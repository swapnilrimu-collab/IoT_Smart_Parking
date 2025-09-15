#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <WebServer.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN 25
#define RST_PIN 27
MFRC522 rfid(SS_PIN, RST_PIN);

#define PI_ADDR 0x08
const int RESPONSE_LENGTH = 8;

char feedbackBuffer[RESPONSE_LENGTH + 1] = {0};
bool dataAvailable = false;

// ---------- NTP (for logs only) ----------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 6*3600, 60000); // GMT+6

// ---------- WiFi ----------
#define WIFI_SSID "Razib Pc Net"
#define WIFI_PASSWORD "1234567890"

// ---------- Firebase ----------
#define API_KEY "AIzaSyDMaJBTj6VJYWSOzCW-Vn5v5wfP_pIO8YQ"
#define DATABASE_URL "https://smartparking-42d60-default-rtdb.firebaseio.com/"
#define USER_EMAIL "swapnilrimu@gmail.com"
#define USER_PASSWORD "Abcd1234#"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------- Sensors ----------
#define SlotSensor1 39
#define SlotSensor2 36
#define SlotSensor3 35
#define SlotSensor4 34

// ---------- LEDs ----------
#define Slot1Red 4
#define Slot1Yellow 2
#define Slot1Green 15
#define Slot2Red 5
#define Slot2Yellow 17
#define Slot2Green 16
#define Slot3Red 14
#define Slot3Yellow 12
#define Slot3Green 13
#define Slot4Red 32
#define Slot4Yellow 33
#define Slot4Green 26

// ---------- Thresholds ----------
#define THRESHOLD1 500
#define THRESHOLD2 500
#define THRESHOLD3 500
#define THRESHOLD4 500

// ---------- Timers / Intervals ----------
unsigned long reservationStart[4] = {0,0,0,0};
const unsigned long reservationTimeout = 60000;   // 1 min for reserved wait
const unsigned long fbUpdateInterval   = 1000;    // 1s -> Firebase status push
const unsigned long localUpdateInterval= 200;     // 200ms -> sensor/LED refresh
const unsigned long messageCheckInterval= 1000;   // 1s -> check reservationRequest
const unsigned long stalePendingMs     = 30000;   // 30s -> consider pending stale
const unsigned long ntpUpdateInterval  = 3600000; // 1 hour -> update NTP time

// ---------- States ----------
int slot1=0, slot2=0, slot3=0, slot4=0;
String carNumbers[4] = {"","","",""};
static bool wasOccupied1=false, wasOccupied2=false, wasOccupied3=false, wasOccupied4=false;
unsigned long lastFirebaseUpdate=0;
unsigned long lastMessageCheck=0;
unsigned long lastNTPUpdate=0;

// Billing (example values)
float reservationFee = 5.0;
float ratePerHour = 20.0;

// Track entry / exit times (epoch seconds)
unsigned long entryTime[4] = {0,0,0,0};
unsigned long exitTime[4]  = {0,0,0,0};

#define MAX_CARS 4
String inside_cars[MAX_CARS]; // array to store car numbers

// Billing via RFID toggling
bool billingActive[4] = {false,false,false,false};

// ---------- De-dupe / anti-ghost ----------
static uint64_t lastProcessedTs = 0;     // last handled reservation timestamp (ms, Date.now())

// ---------- Web server ----------
WebServer server(80);

// ---------- Last bill (for 5s popup on web) ----------
struct LastBill {
  int slot = 0;                // 1..4
  String car = "";
  unsigned long entry = 0;
  unsigned long exitT = 0;
  unsigned long duration = 0;  // seconds
  float reservation = 0;
  float parking = 0;
  float total = 0;
  unsigned long shownAtMs = 0; // millis when created
} lastBill;

// ---------- HTML (same UI) ----------
const char* html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>Smart Parking System</title>
<style>
:root { --available:#4CAF50; --occupied:#F44336; --reserved:#FFC107; --background:#f5f5f5; --card-bg:#ffffff; --text:#333; --led-off:#ddd; --slot-height:220px; }
body{font-family:Segoe UI,Tahoma,Geneva,Verdana,sans-serif;background:var(--background);margin:0;padding:20px;color:var(--text)}
.header{ text-align:center;margin-bottom:30px;padding:20px;background:linear-gradient(135deg,#2c3e50,#3498db);color:#fff;border-radius:8px;box-shadow:0 4px 6px rgba(0,0,0,.1)}
h1{margin:0;font-size:2.2rem}.subtitle{font-weight:normal;opacity:.9}
.dashboard{display:grid;grid-template-columns:repeat(2,1fr);gap:20px;max-width:1200px;margin:0 auto}
.parking-slot{background:var(--card-bg);border-radius:10px;padding:20px;box-shadow:0 2px 10px rgba(0,0,0,.05);transition:.3s;position:relative;overflow:hidden;height:var(--slot-height);display:flex;flex-direction:column;border-top:4px solid transparent}
.parking-slot:hover{transform:translateY(-5px);box-shadow:0 5px 15px rgba(0,0,0,.1)}
.slot-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px}
.slot-title{font-size:1.3rem;font-weight:600;margin:0}
.slot-status{padding:5px 12px;border-radius:20px;font-size:.9rem;font-weight:500;color:#fff}
.available{background:var(--available)}.occupied{background:var(--occupied)}.reserved{background:var(--reserved)}
.led-panel{display:flex;justify-content:center;gap:15px;margin:auto 0}
.led{width:40px;height:40px;border-radius:50%;position:relative;box-shadow:inset 0 2px 5px rgba(0,0,0,.2)}
.led::after{content:'';position:absolute;top:5%;left:10%;width:80%;height:80%;border-radius:50%;background:radial-gradient(circle at 30% 30%, rgba(255,255,255,.8), transparent);opacity:0}
.led.on::after{opacity:.4}
.led-red{background:var(--led-off);border:2px solid #c00}.led-red.on{background:var(--occupied);box-shadow:0 0 15px rgba(244,67,54,.7)}
.led-yellow{background:var(--led-off);border:2px solid #c90}.led-yellow.on{background:var(--reserved);box-shadow:0 0 15px rgba(255,193,7,.7)}
.led-green{background:var(--led-off);border:2px solid #090}.led-green.on{background:var(--available);box-shadow:0 0 15px rgba(76,175,80,.7)}
.led-label{text-align:center;font-size:.8rem;margin-top:5px;color:#666}
.led-container{display:flex;flex-direction:column;align-items:center}
.last-updated{text-align:center;margin-top:30px;font-size:.9rem;color:#777}
.form-container{max-width:600px;margin:20px auto;padding:20px;background:#fff;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,.1)}
.form-container h2{margin-top:0}.form-container label{display:block;margin-bottom:5px;font-weight:500}
.form-container input[type="text"]{width:100%;padding:10px;border:1px solid #ddd;border-radius:4px;font-size:16px;margin-bottom:15px}
.form-container button{background:#3498db;color:#fff;border:none;padding:10px 20px;border-radius:4px;font-size:16px;cursor:pointer}
#form-response{margin-top:15px;padding:10px;border-radius:4px;display:none}
@media (max-width:600px){.dashboard{grid-template-columns:1fr} .header{padding:15px} h1{font-size:1.8rem} .led{width:30px;height:30px} :root{--slot-height:200px}}
/* Small ephemeral bill popup (no layout changes) */
#bill-popup{position:fixed;right:20px;top:20px;background:#fffbe6;border:1px solid #ffd54f;border-radius:10px;padding:14px 16px;box-shadow:0 4px 10px rgba(0,0,0,.15);display:none;z-index:9999}
#bill-popup strong{display:block;margin-bottom:6px}
</style>
</head>
<body>
<div class="header"><h1>Smart Parking System <span class="subtitle">Live Monitoring Panel</span></h1></div>
<div class="dashboard">
  <!-- Four slots (unchanged) -->
  <div class="parking-slot" id="slot1-card"><div class="slot-header"><h2 class="slot-title">Parking Slot 1</h2><span class="slot-status" id="slot1-status">Loading...</span></div>
    <div class="led-panel"><div class="led-container"><div class="led led-red" id="slot1-red"></div><div class="led-label">Red</div></div>
    <div class="led-container"><div class="led led-yellow" id="slot1-yellow"></div><div class="led-label">Yellow</div></div>
    <div class="led-container"><div class="led led-green" id="slot1-green"></div><div class="led-label">Green</div></div></div></div>

  <div class="parking-slot" id="slot2-card"><div class="slot-header"><h2 class="slot-title">Parking Slot 2</h2><span class="slot-status" id="slot2-status">Loading...</span></div>
    <div class="led-panel"><div class="led-container"><div class="led led-red" id="slot2-red"></div><div class="led-label">Red</div></div>
    <div class="led-container"><div class="led led-yellow" id="slot2-yellow"></div><div class="led-label">Yellow</div></div>
    <div class="led-container"><div class="led led-green" id="slot2-green"></div><div class="led-label">Green</div></div></div></div>

  <div class="parking-slot" id="slot3-card"><div class="slot-header"><h2 class="slot-title">Parking Slot 3</h2><span class="slot-status" id="slot3-status">Loading...</span></div>
    <div class="led-panel"><div class="led-container"><div class="led led-red" id="slot3-red"></div><div class="led-label">Red</div></div>
    <div class="led-container"><div class="led led-yellow" id="slot3-yellow"></div><div class="led-label">Yellow</div></div>
    <div class="led-container"><div class="led led-green" id="slot3-green"></div><div class="led-label">Green</div></div></div></div>

  <div class="parking-slot" id="slot4-card"><div class="slot-header"><h2 class="slot-title">Parking Slot 4</h2><span class="slot-status" id="slot4-status">Loading...</span></div>
    <div class="led-panel"><div class="led-container"><div class="led led-red" id="slot4-red"></div><div class="led-label">Red</div></div>
    <div class="led-container"><div class="led led-yellow" id="slot4-yellow"></div><div class="led-label">Yellow</div></div>
    <div class="led-container"><div class="led led-green" id="slot4-green"></div><div class="led-label">Green</div></div></div></div>
</div>

<div class="last-updated" id="last-updated">Last updated: <span id="update-time">Never</span></div>

<div class="form-container">
  <h2>Send Reservation Request</h2>
  <form id="message-form">
    <div><label for="message">Message:</label>
      <input type="text" id="message" name="message" required placeholder="e.g. 1,DHK-202326"/>
    </div>
    <button type="submit">Send Message</button>
  </form>
  <div id="form-response"></div>
</div>

<div id="bill-popup">
  <strong>Bill Generated</strong>
  <div id="bill-text"></div>
</div>

<script>
function updateDashboard(){
  fetch('/status').then(r=>r.json()).then(data=>{
    for(let i=1;i<=4;i++){
      const s=data[`slot${i}`];
      const el=document.getElementById(`slot${i}-status`);
      const r=document.getElementById(`slot${i}-red`);
      const y=document.getElementById(`slot${i}-yellow`);
      const g=document.getElementById(`slot${i}-green`);
      const card=document.getElementById(`slot${i}-card`);
      r.classList.remove('on'); y.classList.remove('on'); g.classList.remove('on');
      card.style.borderTop='4px solid transparent';
      if(s==1){ el.textContent='Occupied'; el.className='slot-status occupied'; r.classList.add('on'); card.style.borderTop='4px solid var(--occupied)'; }
      else if(s==2){ el.textContent='Reserved'; el.className='slot-status reserved'; y.classList.add('on'); card.style.borderTop='4px solid var(--reserved)';}
      else { el.textContent='Available'; el.className='slot-status available'; g.classList.add('on'); card.style.borderTop='4px solid var(--available)';}
    }
    document.getElementById('update-time').textContent=new Date().toLocaleTimeString();
  }).catch(e=>console.error(e));
}
updateDashboard(); setInterval(updateDashboard,1000);

document.getElementById('message-form').addEventListener('submit', async (e)=>{
  e.preventDefault();
  const message=document.getElementById('message').value.trim();
  if(!message) return;
  const resDiv=document.getElementById('form-response');
  resDiv.style.display='block'; resDiv.style.background='#ddd'; resDiv.style.color='#333'; resDiv.textContent='Sending...';
  const hide=()=>setTimeout(()=>resDiv.style.display='none',3000);

  try{
    const res=await fetch('/sendMessage',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'message='+encodeURIComponent(message)});
    const text=await res.text(); let payload=null; try{payload=JSON.parse(text);}catch(e){}
    if(res.ok && payload && payload.ok===true){
      resDiv.style.background='#4CAF50'; resDiv.style.color='#fff';
      resDiv.textContent='✅ '+(payload.message||'Reservation successful.');
      document.getElementById('message').value='';
      updateDashboard();
    }else{
      resDiv.style.background='#F44336'; resDiv.style.color='#fff';
      const msg=(payload&&payload.message)?payload.message:(text||'Reservation failed.');
      resDiv.textContent='❌ '+msg;
    }
    hide();
  }catch(err){
    resDiv.style.background='#F44336'; resDiv.style.color='#fff';
    resDiv.textContent='❌ Error: '+err; hide();
  }
});

// --- Minimal 5s bill popup (no design changes) ---
let billTimer=null;
function pollBill(){
  fetch('/bill').then(r=>r.json()).then(data=>{
    if(data && data.show===true){
      const box=document.getElementById('bill-popup');
      const txt=document.getElementById('bill-text');
      txt.textContent = `${data.slot}: ${data.car} | Duration: ${data.hours}h ${data.minutes}m | Total: $${data.total.toFixed(2)}`;
      box.style.display='block';
      if(billTimer) clearTimeout(billTimer);
      billTimer=setTimeout(()=>{ box.style.display='none'; }, 5000);
    }
  }).catch(e=>{});
}
setInterval(pollBill, 1000);
</script>
</body>
</html>
)rawliteral";

// ---------- Prototypes ----------
void updateSlots();
void sendJsonToFirebase();
void checkFirebaseMessages();
void bootCleanupReservation();
unsigned long getCurrentEpochTime();
void setLEDs(int redPin, int yellowPin, int greenPin, int state);
void processSlot(int idx, int sensorVal, int redPin, int yellowPin, int greenPin, bool &wasOcc, unsigned long currentMillis, unsigned long currentTime);
void generateBill(int idx, unsigned long currentTime);
void clearSlot(int idx);
void syncWithFirebaseStatus();
void rfid_Pi();

// ---------- Helpers for JSON ----------
String jsonEscape(const String &s) {
  String out = s;
  out.replace("\\","\\\\");
  out.replace("\"","\\\"");
  out.replace("\n","\\n");
  out.replace("\r","\\r");
  return out;
}

void setup() {
  Serial.begin(115200);

  // Pins
  pinMode(Slot1Red, OUTPUT); pinMode(Slot1Yellow, OUTPUT); pinMode(Slot1Green, OUTPUT);
  pinMode(Slot2Red, OUTPUT); pinMode(Slot2Yellow, OUTPUT); pinMode(Slot2Green, OUTPUT);
  pinMode(Slot3Red, OUTPUT); pinMode(Slot3Yellow, OUTPUT); pinMode(Slot3Green, OUTPUT);
  pinMode(Slot4Red, OUTPUT); pinMode(Slot4Yellow, OUTPUT); pinMode(Slot4Green, OUTPUT);
  pinMode(SlotSensor1, INPUT); pinMode(SlotSensor2, INPUT); pinMode(SlotSensor3, INPUT); pinMode(SlotSensor4, INPUT);

  // Initialize LEDs to off
  setLEDs(Slot1Red, Slot1Yellow, Slot1Green, -1);
  setLEDs(Slot2Red, Slot2Yellow, Slot2Green, -1);
  setLEDs(Slot3Red, Slot3Yellow, Slot3Green, -1);
  setLEDs(Slot4Red, Slot4Yellow, Slot4Green, -1);

  // I2C (unchanged)
  Wire.begin(PI_ADDR);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent); // called automatically when Pi sends data


  // SPI + RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("ESP32 I2C Slave + RFID ready");

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(" connected!");
  Serial.print("IP address: "); Serial.println(WiFi.localIP());

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Web
  server.on("/", HTTP_GET, [](){ server.send(200,"text/html", html); });

  server.on("/status", HTTP_GET, [](){
    String json="{";
    for(int i=0;i<4;i++){
      int val=0;
      if ((i==0&&slot1==1)||(i==1&&slot2==1)||(i==2&&slot3==1)||(i==3&&slot4==1)) val=1;
      else if (carNumbers[i]!="") val=2; else val=0;
      json += "\"slot"+String(i+1)+"\":"+String(val); if(i<3) json+=",";
    }
    json+="}";
    server.send(200,"application/json",json);
  });

  // Send Reservation Request (unchanged)
  server.on("/sendMessage", HTTP_POST, [](){
    auto sendJson=[&](int code,bool ok,const String&msg,int slot=0,const String&car=""){
      String out="{"; out+="\"ok\":"+(ok?String("true"):String("false"));
      out+=",\"message\":\""+msg+"\"";
      if(slot>0) out+=",\"slot\":"+String(slot);
      if(car.length()>0) out+=",\"car\":\""+jsonEscape(car)+"\"";
      out+="}";
      server.send(code,"application/json",out);
    };

    if(!server.hasArg("message")){ sendJson(400,false,"No message provided."); return; }
    String message=server.arg("message"); message.trim();
    int comma = message.indexOf(','); if(comma==-1){ sendJson(400,false,"Invalid format. Use: slot,car"); return; }
    int slotNum = message.substring(0,comma).toInt(); String car=message.substring(comma+1); car.trim();
    if(slotNum<1||slotNum>4){ sendJson(400,false,"Invalid slot number (1-4)."); return; }
    if(car.length()<3){ sendJson(400,false,"Invalid car number."); return; }
    int idx=slotNum-1;

    // Current occupancy
    int occupied = (idx==0?slot1:(idx==1?slot2:(idx==2?slot3:slot4)));

    if(occupied==1){ sendJson(409,false,"Slot is occupied.",slotNum); return; }
    if(carNumbers[idx]!=""){ sendJson(409,false,"Slot is already reserved.",slotNum); return; }

    carNumbers[idx]=car;
    reservationStart[idx]=millis();
    Serial.printf("[HTTP] Reservation ACCEPTED: Slot %d for %s\n", slotNum, car.c_str());
    sendJson(200,true,"Reservation successful for slot "+String(slotNum)+" ("+car+")",slotNum,car);
  });

  // Bill endpoint for popup (returns show=true for 5s after exit RFID)
  server.on("/bill", HTTP_GET, [](){
    bool show = (millis() - lastBill.shownAtMs) <= 5000 && lastBill.slot>0;
    unsigned long hours = lastBill.duration / 3600;
    unsigned long minutes = (lastBill.duration % 3600) / 60;
    String json = "{";
    json += "\"show\":"; json += (show?"true":"false");
    json += ",\"slot\":"; json += String(lastBill.slot);
    json += ",\"car\":\""; json += jsonEscape(lastBill.car); json += "\"";
    json += ",\"hours\":"; json += String(hours);
    json += ",\"minutes\":"; json += String(minutes);
    json += ",\"total\":"; json += String(lastBill.total,2);
    json += "}";
    server.send(200,"application/json",json);
  });

  server.begin();
  Serial.println("HTTP server started");

  timeClient.begin();
  timeClient.update();
  lastNTPUpdate = millis();

  // Clear any existing reservations on boot
  for(int i = 0; i < 4; i++) {
    reservationStart[i] = 0;
    billingActive[i] = false; // ensure clean billing state
  }

  // Also clear from Firebase to avoid reprocessing
  if(Firebase.ready()) {
    Firebase.RTDB.setString(&fbdo, "/reservationRequest/status", "cleared");
    Firebase.RTDB.setString(&fbdo, "/reservationRequest/message", "Cleared on device boot");
  }

  bootCleanupReservation();
  syncWithFirebaseStatus();
}

void receiveEvent(int bytes) {
  String carNumber = "";
  while(Wire.available()) {
    char c = Wire.read();
    carNumber += c; // build car number string
  }

  // store in first empty slot
  for(int i = 0; i < 4; i++) {
    if(inside_cars[i] == "") { 
      inside_cars[i] = carNumber;
      break; // stop after storing
    }
  }
}


void loop() {
  server.handleClient();

  unsigned long now = millis();

  rfid_Pi(); // <-- RFID controls billing start/stop

  // Update NTP time periodically
  if(now - lastNTPUpdate >= ntpUpdateInterval) {
    timeClient.update();
    lastNTPUpdate = now;
    Serial.println("NTP time updated");
  }

  if(now - lastFirebaseUpdate >= fbUpdateInterval){
    lastFirebaseUpdate = now;
    sendJsonToFirebase();
  }

  if(now - lastMessageCheck >= messageCheckInterval){
    lastMessageCheck = now;
    checkFirebaseMessages();
  }

  updateSlots();
  delay(10);
}

// ---------- RFID-driven billing ----------
void rfid_Pi(){
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String rfidUID = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) rfidUID += "0";
      rfidUID += String(rfid.uid.uidByte[i], HEX);
    }
    rfidUID.toUpperCase();

    // Map UID to slot ID (1..4)
    String cardID = "0";
    if (rfidUID == "9ECD2303") cardID = "1";
    else if (rfidUID == "130FC711") cardID = "2";
    else if (rfidUID == "33DD94EC") cardID = "3";
    else if (rfidUID == "4333F811") cardID = "4";

    // Prepare the buffer (I2C unchanged)
    memset(feedbackBuffer, 0, sizeof(feedbackBuffer));
    strncpy(feedbackBuffer, cardID.c_str(), RESPONSE_LENGTH);
    dataAvailable = true;

    Serial.print("Card scanned: UID=");
    Serial.print(rfidUID);
    Serial.print(" -> ID=");
    Serial.println(cardID);

    // ---- Billing toggle by RFID ----
    int slotNum = cardID.toInt(); // 0 if unknown
    if(slotNum >= 1 && slotNum <= 4){
      int idx = slotNum - 1;
      unsigned long nowEpoch = getCurrentEpochTime();

      if(!billingActive[idx]){
        // ENTRY scan -> start timer
        billingActive[idx] = true;
        entryTime[idx] = nowEpoch;
        Serial.printf("[RFID] Entry started for Slot %d at %lu\n", slotNum, entryTime[idx]);
      } else {
        // EXIT scan -> stop timer & bill
        billingActive[idx] = false;
        generateBill(idx, nowEpoch);
        clearSlot(idx); // free the reservation & reset times
      }
    }

    rfid.PICC_HaltA();
  }

  delay(50);
}

// I2C request (unchanged)
void requestEvent() {
  if (dataAvailable) {
    Wire.write((const uint8_t*)feedbackBuffer, RESPONSE_LENGTH);
    Serial.print("Sent to Pi: ");
    Serial.println(feedbackBuffer);
    memset(feedbackBuffer, 0, sizeof(feedbackBuffer));
    dataAvailable = false;
  } else {
    Wire.write(0); // single null byte
  }
}

// ---------- Improved time handling ----------
unsigned long getCurrentEpochTime() {
  return timeClient.getEpochTime();
}

void syncWithFirebaseStatus() {
  if(Firebase.RTDB.getJSON(&fbdo, "/parkingStatus")) {
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData carData;
    for(int i = 0; i < 4; i++) {
      String path = "car" + String(i+1);
      if(json.get(carData, path.c_str())) {
        carNumbers[i] = carData.stringValue;
        Serial.printf("Synced slot %d car: %s\n", i+1, carNumbers[i].c_str());
      }
    }
  }
}

// ---------- Improved boot cleanup ----------
void bootCleanupReservation(){
  Serial.println("[Boot] Checking for existing reservations...");
  if(Firebase.RTDB.getJSON(&fbdo, "/reservationRequest")){
    FirebaseJson req = fbdo.jsonObject();
    FirebaseJsonData sData, tData;
    if(req.get(sData,"status") && sData.stringValue=="pending"){
      if(req.get(tData,"timestamp")){
        uint64_t ts = (uint64_t)tData.doubleValue;
        uint64_t currentTimeMs = getCurrentEpochTime() * 1000;
        if(ts + stalePendingMs < currentTimeMs){
          Firebase.RTDB.setString(&fbdo, "/reservationRequest/status", "expired");
          Firebase.RTDB.setString(&fbdo, "/reservationRequest/message", "Auto-expired on device boot");
          Serial.println("[Boot] Expired stale pending reservation.");
        }
      }
    } else {
      Serial.println("[Boot] No pending reservations found");
    }
  } else {
    Serial.println("No reservation request found or error reading from Firebase");
  }
}

// ---------- Simplified LED control function ----------
void setLEDs(int redPin, int yellowPin, int greenPin, int state) {
  // state: 1=red, 2=yellow, 0=green, -1=all off
  if(state == -1){
    digitalWrite(redPin, LOW);
    digitalWrite(yellowPin, LOW);
    digitalWrite(greenPin, LOW);
    return;
  }
  digitalWrite(redPin, (state == 1) ? HIGH : LOW);
  digitalWrite(yellowPin, (state == 2) ? HIGH : LOW);
  digitalWrite(greenPin, (state == 0) ? HIGH : LOW);
}

// ---------- Update slots (no billing here; RFID-only billing) ----------
void updateSlots() {
  static unsigned long lastLocalUpdate=0;
  unsigned long now = millis();
  if(now - lastLocalUpdate < localUpdateInterval) return;
  lastLocalUpdate = now;

  // Read sensors
  slot1 = (analogRead(SlotSensor1) < THRESHOLD1) ? 1 : 0;
  slot2 = (analogRead(SlotSensor2) < THRESHOLD2) ? 1 : 0;
  slot3 = (analogRead(SlotSensor3) < THRESHOLD3) ? 1 : 0;
  slot4 = (analogRead(SlotSensor4) < THRESHOLD4) ? 1 : 0;

  unsigned long currentTime = getCurrentEpochTime();

  // LEDs + reservation timing only; do NOT generate bills here
  processSlot(0, slot1, Slot1Red, Slot1Yellow, Slot1Green, wasOccupied1, now, currentTime);
  processSlot(1, slot2, Slot2Red, Slot2Yellow, Slot2Green, wasOccupied2, now, currentTime);
  processSlot(2, slot3, Slot3Red, Slot3Yellow, Slot3Green, wasOccupied3, now, currentTime);
  processSlot(3, slot4, Slot4Red, Slot4Yellow, Slot4Green, wasOccupied4, now, currentTime);
}

void processSlot(int idx, int sensorVal, int redPin, int yellowPin, int greenPin, bool &wasOcc, unsigned long currentMillis, unsigned long currentTime) {
  bool isReserved = (carNumbers[idx] != "");
  int slotNum = idx + 1;

  if(sensorVal == 1) { // Occupied
    setLEDs(redPin, yellowPin, greenPin, 1);
    wasOcc = true;
    reservationStart[idx] = 0; // reservation fulfilled
  } else { // Not occupied
    if(isReserved) {
      setLEDs(redPin, yellowPin, greenPin, 2);
      if(reservationStart[idx] == 0) {
        reservationStart[idx] = currentMillis;
      } else if(currentMillis - reservationStart[idx] >= reservationTimeout) {
        clearSlot(idx);
        setLEDs(redPin, yellowPin, greenPin, 0);
        Serial.printf("Slot %d reservation timed out.\n", slotNum);
      }
    } else {
      setLEDs(redPin, yellowPin, greenPin, 0);
    }
    wasOcc = false;
  }
}

void generateBill(int idx, unsigned long currentTime) {
  exitTime[idx] = currentTime;

  // Safety: if no entry time, ignore
  if(entryTime[idx] == 0){
    Serial.printf("[Bill] Ignored: Slot %d had no entry time\n", idx+1);
    return;
  }

  // Calculate parking duration (in seconds)
  unsigned long parkingDuration = (exitTime[idx] >= entryTime[idx]) ? (exitTime[idx] - entryTime[idx]) : 0;

  // Convert to hours and minutes
  unsigned long hours = parkingDuration / 3600;
  unsigned long minutes = (parkingDuration % 3600) / 60;

  // Calculate fees
  float parkingFee = (parkingDuration / 3600.0f) * ratePerHour;
  float total = parkingFee;
  String carInfo = "Unknown";
  if(carNumbers[idx] != "") {
    total += reservationFee;
    carInfo = carNumbers[idx];
  } else {
    carInfo = String("Card-Slot-") + String(idx+1);
  }

  Serial.println("\n==============================================");
  Serial.printf("            BILL FOR SLOT %d\n", idx + 1);
  Serial.println("==============================================");
  Serial.printf("Car Number:        %s\n", carInfo.c_str());
  Serial.printf("Entry Time:        %s", ctime((time_t*)&entryTime[idx]));
  Serial.printf("Exit Time:         %s", ctime((time_t*)&exitTime[idx]));
  Serial.printf("Parking Duration:  %luh %lum\n", hours, minutes);
  if(carNumbers[idx] != "") {
    Serial.printf("Reservation Fee:   $%.2f\n", reservationFee);
  }
  Serial.printf("Parking Fee:       $%.2f\n", parkingFee);
  Serial.printf("Total Amount:      $%.2f\n", total);
  Serial.println("==============================================\n");

  // ---------- Save to Firebase ----------
  if(Firebase.ready()) {
    String slotPath = "/bills/slot" + String(idx + 1) + "/last";

    FirebaseJson bill;
    bill.set("slot", idx+1);
    bill.set("car", carInfo);
    bill.set("entryTime", (int)entryTime[idx]);
    bill.set("exitTime",  (int)exitTime[idx]);
    bill.set("durationSeconds", (int)parkingDuration);
    bill.set("reservationFee", (carNumbers[idx] != "") ? reservationFee : 0);
    bill.set("parkingFee", parkingFee);
    bill.set("total", total);

    // Set last
    Firebase.RTDB.setJSON(&fbdo, slotPath.c_str(), &bill);

    // Push to history
    Firebase.RTDB.pushJSON(&fbdo, "/bills/history", &bill);
  }

  // ---------- Set local lastBill for 5s popup ----------
  lastBill.slot = idx+1;
  lastBill.car = carInfo;
  lastBill.entry = entryTime[idx];
  lastBill.exitT = exitTime[idx];
  lastBill.duration = parkingDuration;
  lastBill.reservation = (carNumbers[idx] != "") ? reservationFee : 0;
  lastBill.parking = parkingFee;
  lastBill.total = total;
  lastBill.shownAtMs = millis();
}

void clearSlot(int idx) {
  // Do NOT clear carNumbers if you want to keep reservation for next time.
  // Since exit scan means leaving, clear it to free the slot:
  carNumbers[idx] = "";
  entryTime[idx] = 0;
  exitTime[idx] = 0;
  reservationStart[idx] = 0;
  billingActive[idx] = false;
}

// ---------- Push current state to Firebase ----------
void sendJsonToFirebase() {
  FirebaseJson json;

  int s1 = (slot1==1) ? 1 : (carNumbers[0]!="" ? 2:0);
  int s2 = (slot2==1) ? 1 : (carNumbers[1]!="" ? 2:0);
  int s3 = (slot3==1) ? 1 : (carNumbers[2]!="" ? 2:0);
  int s4 = (slot4==1) ? 1 : (carNumbers[3]!="" ? 2:0);

  json.set("slot1", s1);
  json.set("slot2", s2);
  json.set("slot3", s3);
  json.set("slot4", s4);
  json.set("car1", carNumbers[0]);
  json.set("car2", carNumbers[1]);
  json.set("car3", carNumbers[2]);
  json.set("car4", carNumbers[3]);
  json.set("lastUpdate", getCurrentEpochTime());

  if(Firebase.RTDB.setJSON(&fbdo, "/parkingStatus", &json)){
    // ok
  }else{
    Serial.printf("Firebase update failed: %s\n", fbdo.errorReason().c_str());
  }
}

// ---------- Check Firebase reservationRequest safely ----------
void checkFirebaseMessages() {
  if(!Firebase.ready()) return;

  if(Firebase.RTDB.getJSON(&fbdo, "/reservationRequest")){
    FirebaseJson req = fbdo.jsonObject();
    FirebaseJsonData statusData, slotData, carData, tsData;

    bool okStatus = req.get(statusData,"status");
    bool okSlot   = req.get(slotData,"slot");
    bool okCar    = req.get(carData,"carNumber");
    bool okTs     = req.get(tsData,"timestamp");     // IMPORTANT

    if(!(okStatus && okSlot && okCar && okTs)) return;

    String status = statusData.stringValue;
    int slotNum   = slotData.intValue;
    String carNum = carData.stringValue;
    uint64_t ts   = (uint64_t) tsData.doubleValue;   // JS Date.now() (ms)

    if(status != "pending") return;
    if(slotNum < 1 || slotNum > 4) return;

    if(ts == 0 || ts <= lastProcessedTs){
      return;
    }

    uint64_t currentTimeMs = getCurrentEpochTime() * 1000;
    if(ts + stalePendingMs < currentTimeMs){
      Firebase.RTDB.setString(&fbdo, "/reservationRequest/status", "expired");
      Firebase.RTDB.setString(&fbdo, "/reservationRequest/message", "Request expired - too old");
      lastProcessedTs = ts;
      return;
    }

    int idx = slotNum - 1;
    bool isOccupied = (idx==0&&slot1==1)||(idx==1&&slot2==1)||(idx==2&&slot3==1)||(idx==3&&slot4==1);

    if(!isOccupied && carNumbers[idx]==""){
      // Accept
      carNumbers[idx]=carNum;
      reservationStart[idx]=millis();
      lastProcessedTs = ts;
      Serial.printf("[Firebase] Reservation ACCEPTED: Slot %d for %s\n", slotNum, carNum.c_str());

      Firebase.RTDB.setString(&fbdo, "/reservationRequest/status", "accepted");
      Firebase.RTDB.setString(&fbdo, "/reservationRequest/message", "Reservation successful");
      Firebase.RTDB.setDouble(&fbdo, "/reservationRequest/processedAt", (double)getCurrentEpochTime());
    }else{
      // Reject
      String reason = isOccupied ? "Slot is occupied" : "Slot is already reserved";
      lastProcessedTs = ts;
      Serial.printf("[Firebase] Reservation REJECTED: Slot %d (%s)\n", slotNum, reason.c_str());

      Firebase.RTDB.setString(&fbdo, "/reservationRequest/status", "rejected");
      Firebase.RTDB.setString(&fbdo, "/reservationRequest/message", reason);
      Firebase.RTDB.setDouble(&fbdo, "/reservationRequest/processedAt", (double)getCurrentEpochTime());
    }
  } else {
    Serial.println("Failed to read from reservationRequest path");
  }
}
