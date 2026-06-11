// ============================================================
//   VAT LEACHING SYSTEM – ESP32 (with GSM SIM800C)
//   Mega → Serial1 (RX=GPIO26, TX=GPIO27) @ 9600
//   MQTT → broker.hivemq.com:1883 (public)
//   GSM  → Serial2 (RX=GPIO16, TX=GPIO17) @ 115200
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ================= WI-FI =================
const char* ssid     = "MCHILI'S_PC";
const char* password = "87654321";

// ================= PUBLIC MQTT BROKER =================
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

// ================= MQTT TOPICS =================
const char* topic_publish   = "vatleaching/device1/data";
const char* topic_subscribe = "vatleaching/device1/cmd";

// ================= SMS NUMBERS (6) =================
#define SMS_COUNT 6
const char* SMS_NUMBERS[SMS_COUNT] = {
  "+255637341780",
  "+255692007363",
  "+255625200264",
  "+255687947822",
  "+255783712629",
  "+255676544740"
};

// ================= AUTHORISED CALLER (your number) =================
const char* AUTHORISED_NUMBER = "+255743278973";

// ================= SERIAL PORTS =================
HardwareSerial MegaSerial(1);   // RX=GPIO26, TX=GPIO27 (Mega)
HardwareSerial GsmSerial(2);    // RX=GPIO16, TX=GPIO17 (SIM800C)

// ================= SENSOR GLOBALS =================
float  g_d1   = 0;
float  g_d2   = 0;
float  g_ph   = 7.0;
float  g_wl1  = 0;
float  g_wl2  = 0;
float  g_vol  = 0;
float  g_lime = 0;
String g_pump = "OFF";

// ================= GSM STATE =================
bool   gsmOK           = false;
bool   smsSentForCycle = false;
String lastPumpState   = "OFF";
bool   processingCall  = false;

// ================= MQTT CLIENT =================
WiFiClient espClient;
PubSubClient client(espClient);

// ================= TIMING =================
unsigned long lastPublish    = 0;
unsigned long lastReconnect  = 0;
unsigned long startTime      = 0;
const long    publishInterval   = 5000;
const long    reconnectInterval = 5000;

// ============================================================
// UPTIME
// ============================================================
String getUptime() {
  unsigned long secs = (millis() - startTime) / 1000;
  char buf[12];
  sprintf(buf, "%02lu:%02lu:%02lu", secs/3600, (secs%3600)/60, secs%60);
  return String(buf);
}

// ============================================================
// GSM HELPERS
// ============================================================
bool gsmWaitFor(const char* expected, unsigned long timeout = 3000) {
  unsigned long t0 = millis();
  String resp = "";
  while (millis() - t0 < timeout) {
    while (GsmSerial.available()) {
      char c = GsmSerial.read();
      resp += c;
      if (resp.indexOf(expected) >= 0) return true;
    }
  }
  if (resp.length() > 0) {
    Serial.print("[GSM] Expected '"); Serial.print(expected);
    Serial.print("' | Got: "); Serial.println(resp);
  }
  return false;
}

void gsmSend(const char* cmd) {
  GsmSerial.println(cmd);
  delay(100);
}

bool gsmInit() {
  Serial.println("[GSM] Initialising SIM800C at 115200 baud...");
  // Power‑on delay – SIM800C can take up to 10 seconds
  delay(10000);
  // Flush any garbage
  while (GsmSerial.available()) GsmSerial.read();

  gsmSend("AT");
  if (!gsmWaitFor("OK", 3000)) {
    Serial.println("[GSM] No AT response – check wiring/power");
    return false;
  }
  Serial.println("[GSM] AT OK");

  gsmSend("AT+IPR=115200");   // lock baud rate
  gsmWaitFor("OK", 1000);
  gsmSend("ATE0");            // echo off
  gsmWaitFor("OK", 1000);
  gsmSend("AT+CMGF=1");       // SMS text mode
  if (!gsmWaitFor("OK", 2000)) return false;
  gsmSend("AT+CNMI=1,2,0,0,0"); // forward SMS to serial
  gsmWaitFor("OK", 1000);
  gsmSend("AT+CLIP=1");       // enable caller ID
  gsmWaitFor("OK", 1000);
  gsmSend("AT+CSCS=\"GSM\""); // character set
  gsmWaitFor("OK", 1000);
  gsmSend("AT+CPIN?");        // check SIM
  if (!gsmWaitFor("READY", 5000)) {
    Serial.println("[GSM] SIM not ready – check SIM card");
    return false;
  }
  gsmSend("AT+CSQ");          // signal quality
  gsmWaitFor("OK", 2000);

  Serial.println("[GSM] SIM800C ready");
  return true;
}

void sendSMS(const char* number, const String& message) {
  if (!gsmOK) return;
  Serial.print("[GSM] Sending SMS to "); Serial.println(number);
  GsmSerial.print("AT+CMGS=\"");
  GsmSerial.print(number);
  GsmSerial.println("\"");
  if (!gsmWaitFor(">", 4000)) {
    Serial.print("[GSM] No prompt for "); Serial.println(number);
    return;
  }
  GsmSerial.print(message);
  GsmSerial.write(26);          // Ctrl+Z
  delay(200);
  if (gsmWaitFor("OK", 10000))
    Serial.print("[GSM] SMS sent -> ");
  else
    Serial.print("[GSM] SMS FAILED -> ");
  Serial.println(number);
  delay(500);
}

void sendSMSToAll(const String& message) {
  Serial.println("[GSM] Sending SMS to all 6 numbers...");
  for (int i = 0; i < SMS_COUNT; i++) {
    sendSMS(SMS_NUMBERS[i], message);
  }
  Serial.println("[GSM] SMS burst complete.");
}

String buildSMSMessage() {
  String msg = "VAT LEACHING SYSTEM\n====================\n";
  msg += "STATUS: PUMP " + g_pump + "\n\nUPPER TANK\n";
  msg += "  Dist: " + String(g_d1,1) + " cm\n  Level: " + String(g_wl1,1) + " cm\n\nLOWER TANK\n";
  msg += "  Dist: " + String(g_d2,1) + " cm\n  Level: " + String(g_wl2,1) + " cm\n";
  msg += "  Vol: " + String(g_vol,2) + " L\n  Lime: " + String(g_lime,1) + " g\n\npH: " + String(g_ph,2);
  return msg;
}

void handleIncomingCall() {
  while (GsmSerial.available()) {
    String line = GsmSerial.readStringUntil('\n');
    line.trim();
    if (line.indexOf("RING") >= 0) {
      Serial.println("[GSM] Incoming call detected");
      processingCall = true;
    }
    if (line.indexOf("+CLIP:") >= 0 && processingCall) {
      int startQuote = line.indexOf('"');
      int endQuote = line.indexOf('"', startQuote + 1);
      if (startQuote >= 0 && endQuote > startQuote) {
        String caller = line.substring(startQuote + 1, endQuote);
        Serial.print("[GSM] Call from "); Serial.println(caller);
        if (caller == AUTHORISED_NUMBER) {
          Serial.println("[GSM] Authorised caller – sending status SMS");
          sendSMS(AUTHORISED_NUMBER, buildSMSMessage());
          gsmSend("ATA");   // answer
          delay(1000);
          gsmSend("ATH");   // hang up
        } else {
          Serial.println("[GSM] Unknown caller – rejecting");
          gsmSend("ATH");
        }
        processingCall = false;
      }
    }
  }
}

// ============================================================
// WI-FI
// ============================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] FAILED");
  }
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.print("[MQTT] CMD: "); Serial.println(message);
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) return;
  if (doc.containsKey("pump")) {
    String val = doc["pump"].as<String>();
    MegaSerial.print("PUMP,"); MegaSerial.println(val);
    Serial.print("[CMD] Pump -> "); Serial.println(val);
  }
}

bool reconnectMQTT() {
  if (client.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return false; }
  unsigned long now = millis();
  if (now - lastReconnect < reconnectInterval) return false;
  lastReconnect = now;
  Serial.print("[MQTT] Connecting to public broker... ");
  String clientId = "ESP32_VAT_" + String(random(0xffff), HEX);
  if (client.connect(clientId.c_str())) {
    Serial.println("Connected!");
    client.subscribe(topic_subscribe);
    return true;
  }
  Serial.print("Failed rc="); Serial.println(client.state());
  return false;
}

void publishData() {
  if (!client.connected()) return;
  StaticJsonDocument<512> doc;
  doc["d1"]    = g_d1;
  doc["d2"]    = g_d2;
  doc["ph"]    = g_ph;
  doc["wl1"]   = g_wl1;
  doc["wl2"]   = g_wl2;
  doc["vol"]   = g_vol;
  doc["lime"]  = g_lime;
  doc["pump"]  = g_pump;
  doc["uptime"] = getUptime();
  char buffer[256];
  serializeJson(doc, buffer);
  if (client.publish(topic_publish, buffer)) {
    Serial.print("[MQTT] Published | pump:"); Serial.print(g_pump);
    Serial.print(" pH:"); Serial.print(g_ph);
    Serial.print(" vol:"); Serial.println(g_vol);
  } else {
    Serial.println("[MQTT] Publish FAILED");
  }
}

// ============================================================
// PARSE MEGA DATA (8 fields, robust)
// ============================================================
void parseMegaData() {
  static String buffer = "";
  while (MegaSerial.available()) {
    char c = MegaSerial.read();
    if (c == '\n') {
      if (buffer.length() > 0) {
        // Debug raw line
        Serial.print("[RAW] ");
        Serial.println(buffer);
        if (buffer.startsWith("DATA,")) {
          String data = buffer.substring(5);
          int fieldCount = 1;
          for (int i = 0; i < data.length(); i++) if (data[i] == ',') fieldCount++;
          if (fieldCount == 8) {
            String parts[8];
            int idx = 0, start = 0;
            for (int i = 0; i <= data.length(); i++) {
              if (i == data.length() || data[i] == ',') {
                parts[idx++] = data.substring(start, i);
                start = i + 1;
                if (idx >= 8) break;
              }
            }
            g_d1   = parts[0].toFloat();
            g_d2   = parts[1].toFloat();
            g_ph   = parts[2].toFloat();
            g_wl1  = parts[3].toFloat();
            g_wl2  = parts[4].toFloat();
            g_vol  = parts[5].toFloat();
            g_lime = parts[6].toFloat();
            g_pump = parts[7];
            g_pump.trim();
            Serial.print("[UART] d1="); Serial.print(g_d1);
            Serial.print(" d2=");       Serial.print(g_d2);
            Serial.print(" pH=");       Serial.print(g_ph);
            Serial.print(" vol=");      Serial.print(g_vol);
            Serial.print(" pump=");     Serial.println(g_pump);

            // SMS trigger on OFF → ON transition
            if (lastPumpState == "OFF" && g_pump == "ON") {
              smsSentForCycle = false;
            }
            if (g_pump == "ON" && !smsSentForCycle) {
              sendSMSToAll(buildSMSMessage());
              smsSentForCycle = true;
            }
            lastPumpState = g_pump;
          } else {
            Serial.printf("[UART] ERROR: expected 8 fields, got %d\n", fieldCount);
          }
        }
      }
      buffer = "";
    } else {
      buffer += c;
      if (buffer.length() > 200) buffer = "";
    }
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  MegaSerial.begin(9600, SERIAL_8N1, 26, 27);   // Mega
  GsmSerial.begin(115200, SERIAL_8N1, 16, 17);  // SIM800C

  Serial.println("\n========================================");
  Serial.println("   VAT LEACHING SYSTEM — ESP32");
  Serial.println("========================================");
  Serial.println("Mega  : Serial1 RX=GPIO26 TX=GPIO27 @ 9600");
  Serial.println("GSM   : Serial2 RX=GPIO16 TX=GPIO17 @ 115200");
  Serial.println("MQTT  : broker.hivemq.com:1883 (public)");
  Serial.println("========================================\n");

  // Initialise GSM
  gsmOK = gsmInit();
  if (!gsmOK) {
    Serial.println("[GSM] WARNING: offline – SMS/call disabled");
  } else {
    sendSMS(AUTHORISED_NUMBER, "VAT Leaching System ONLINE!");
  }

  // Connect to Wi‑Fi and MQTT
  connectWiFi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  client.setKeepAlive(60);
  int tries = 0;
  while (!reconnectMQTT() && tries < 5) {
    delay(reconnectInterval);
    tries++;
  }

  startTime = millis();
  Serial.println("[SYS] Ready! Waiting for Mega data...\n");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // MQTT keep‑alive
  if (!client.connected()) reconnectMQTT();
  else client.loop();

  // Handle incoming GSM calls
  if (gsmOK) handleIncomingCall();

  // Read data from Mega
  parseMegaData();

  // Publish to MQTT every 5 seconds
  if (millis() - lastPublish >= publishInterval) {
    publishData();
    lastPublish = millis();
  }

  delay(10);
}