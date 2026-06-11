// ============================================================
//   VAT LEACHING SYSTEM – MEGA
//   Upper tank: A0221AU (UART, max 22 cm)
//   Lower tank: HC‑SR04 (pins 28/30), empty at 22 cm, full at 0 cm (water level = 22 - distance)
//   pH sensor: A0 with offset to read 7.0 in water
// ============================================================

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <math.h>
#include <EEPROM.h>

// ================= DISPLAY =================
#define TFT_CS   53
#define TFT_DC    9
#define TFT_RST   8
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// ================= SENSORS =================
#define TRIG_HC   28
#define ECHO_HC   30
#define PH_PIN    A0

// ================= RELAY =================
#define RELAY     6

// ================= TANK PARAMETERS =================
#define TANK1_HEIGHT         22.0f          // Upper tank max water height (cm)
#define TANK1_SHUTDOWN_DIST   5.0f          // Distance when pump stops (cm)
#define TANK1_START_DIST      8.0f          // Distance when pump resumes (cm)

// Lower tank: sensor mounted 22 cm above bottom
#define TANK2_EMPTY_DIST     22.0f          // Distance when tank empty (cm)
#define TANK2_MAX_HEIGHT     22.0f          // Max water height (cm)
#define TANK2_RADIUS         20.0f          // Tank radius (cm)
#define VOLUME_CAL            1.25f

// ================= SCREEN =================
#define SCREEN_W 320
#define SCREEN_H 240

// ================= pH OFFSET =================
#define PH_OFFSET            -4.73f

// ================= EEPROM ADDRESSES =================
#define EE_SLOPE     0
#define EE_INTERCEPT 4
#define EE_MAGIC     8
const uint16_t MAGIC_NUMBER = 0xCAFE;

// ================= GLOBALS =================
float  d1           = 0;
float  d2           = 0;
float  ph           = 7.0f;
bool   pumpShutdown = false;
int    waveMotion   = 0;
bool   blink        = false;
bool   firstDraw    = true;

bool          lastPumpState = false;
float         lastPH        = 0;
unsigned long lastSend      = 0;

// pH calibration constants
float ph_slope     = -0.018;
float ph_intercept = 0.0f;
bool  ph_calibrated = false;

// Moving average for pH
#define PH_AVG_WINDOW 5
float phBuffer[PH_AVG_WINDOW];
int   phIndex = 0;
float phSum   = 0;
bool  phBufferFull = false;

// =====================================================
// pH SOFTWARE CALIBRATION (stored in EEPROM)
// =====================================================
void loadPHCalibration() {
  uint16_t magic;
  EEPROM.get(EE_MAGIC, magic);
  if (magic == MAGIC_NUMBER) {
    EEPROM.get(EE_SLOPE, ph_slope);
    EEPROM.get(EE_INTERCEPT, ph_intercept);
    ph_calibrated = true;
    Serial.println("[pH] Calibration loaded from EEPROM");
  } else {
    // Default: ADC 345 → pH 7.0 before offset
    ph_slope = -0.018;
    ph_intercept = 7.0 - ph_slope * 345;   // = 13.21
    ph_calibrated = false;
    Serial.println("[pH] No calibration found. Using default, offset will be applied.");
  }
}

void savePHCalibration(float slope, float intercept) {
  EEPROM.put(EE_SLOPE, slope);
  EEPROM.put(EE_INTERCEPT, intercept);
  EEPROM.put(EE_MAGIC, MAGIC_NUMBER);
  ph_slope = slope;
  ph_intercept = intercept;
  ph_calibrated = true;
  Serial.println("[pH] Calibration saved to EEPROM");
}

// Median filter (15 samples) + moving average + offset
float readPH() {
  const int numSamples = 15;
  int samples[numSamples];
  for (int i = 0; i < numSamples; i++) {
    samples[i] = analogRead(PH_PIN);
    delay(10);
  }
  // Sort for median
  for (int i = 0; i < numSamples - 1; i++) {
    for (int j = i + 1; j < numSamples; j++) {
      if (samples[j] < samples[i]) {
        int t = samples[i]; samples[i] = samples[j]; samples[j] = t;
      }
    }
  }
  int raw = samples[numSamples / 2];
  
  float phVal = ph_slope * raw + ph_intercept;
  phVal = constrain(phVal, 0, 14);
  
  // Moving average
  phSum -= phBuffer[phIndex];
  phBuffer[phIndex] = phVal;
  phSum += phVal;
  phIndex = (phIndex + 1) % PH_AVG_WINDOW;
  if (!phBufferFull && phIndex == 0) phBufferFull = true;
  int count = phBufferFull ? PH_AVG_WINDOW : phIndex;
  float filteredPH = phSum / count;
  
  // Apply user offset to make water read 7.0
  filteredPH += PH_OFFSET;
  return constrain(filteredPH, 0, 14);
}

// =====================================================
// SENSOR READ FUNCTIONS
// =====================================================
float readUART_Ultrasonic() {
  while (Serial1.available()) Serial1.read();
  Serial1.write(0x01);
  unsigned long t0 = millis();
  uint8_t buf[4];
  int idx = 0;
  while (millis() - t0 < 150) {
    if (!Serial1.available()) continue;
    uint8_t b = Serial1.read();
    if (idx == 0 && b != 0xFF) continue;
    buf[idx++] = b;
    if (idx < 4) continue;
    uint8_t cs = (buf[0] + buf[1] + buf[2]) & 0xFF;
    if (cs != buf[3]) { idx = 0; continue; }
    uint16_t mm = ((uint16_t)buf[1] << 8) | buf[2];
    return mm / 10.0f;
  }
  return 0;
}

float readHC_SR04(int trig, int echo) {
  float samples[5];
  int valid = 0;
  for (int i = 0; i < 5; i++) {
    digitalWrite(trig, LOW);
    delayMicroseconds(4);
    digitalWrite(trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(trig, LOW);
    long dur = pulseIn(echo, HIGH, 40000);
    if (dur > 0) {
      float dist = (dur * 0.0343f) / 2.0f;
      if (dist >= 2.0f && dist <= 450.0f) samples[valid++] = dist;
    }
    delay(30);
  }
  if (valid == 0) return 0;
  if (valid == 1) return samples[0];
  for (int i = 0; i < valid-1; i++)
    for (int j = i+1; j < valid; j++)
      if (samples[j] < samples[i]) {
        float t = samples[i]; samples[i] = samples[j]; samples[j] = t;
      }
  return samples[valid/2];
}

// =====================================================
// CALCULATIONS
// =====================================================
float getWaterLevel_T1(float dist) {
  if (dist <= 0) return 0;
  return constrain(TANK1_HEIGHT - dist, 0, TANK1_HEIGHT);
}

// Lower tank: water level = empty_dist - measured distance, clamped 0..max_height
float getWaterLevel_T2(float dist) {
  if (dist <= 0) return TANK2_MAX_HEIGHT;   // full (water touches sensor)
  float level = TANK2_EMPTY_DIST - dist;
  if (level < 0) level = 0;
  if (level > TANK2_MAX_HEIGHT) level = TANK2_MAX_HEIGHT;
  return level;
}

float getVolume_T2(float waterLevel) {
  return (PI * TANK2_RADIUS * TANK2_RADIUS * waterLevel / 1000.0f) * VOLUME_CAL;
}

float getLimeMass(float phVal, float volumeLiters) {
  if (phVal >= 12.0f) return 0;
  return constrain(((12.0f - phVal) / 10.0f) * volumeLiters, 0, 99999.9f);
}

// =====================================================
// SEND TO ESP32 (8 fields, LF only)
// =====================================================
void sendToESP32(float wl1, float wl2, float vol, float lime) {
  if (millis() - lastSend < 2000) return;
  lastSend = millis();

  String pkt = "DATA,";
  pkt += String(d1,   2); pkt += ",";
  pkt += String(d2,   2); pkt += ",";
  pkt += String(ph,   2); pkt += ",";
  pkt += String(wl1,  2); pkt += ",";
  pkt += String(wl2,  2); pkt += ",";
  pkt += String(vol,  2); pkt += ",";
  pkt += String(lime, 1); pkt += ",";
  pkt += (pumpShutdown ? "OFF" : "ON");

  Serial2.print(pkt);
  Serial2.write('\n');
  Serial2.flush();

  Serial.print("[TX→ESP] "); Serial.println(pkt);

  // Notifications
  if (pumpShutdown != lastPumpState) {
    String notif = pumpShutdown ? "NOTIF,PUMP_OFF,Pump OFF - dist " : "NOTIF,PUMP_ON,Pump ON - dist ";
    notif += String(d1, 1); notif += "cm";
    Serial2.print(notif); Serial2.write('\n'); Serial2.flush();
    lastPumpState = pumpShutdown;
  }
  if (ph >= 10.5f && ph <= 12.0f && abs(ph - lastPH) > 0.1f) {
    String notif = "NOTIF,PH_HIGH,pH=" + String(ph,2) + " high alkaline";
    Serial2.print(notif); Serial2.write('\n'); Serial2.flush();
    lastPH = ph;
  }
  if (ph > 12.0f) {
    String alarm = "ALARM,PH_CRITICAL,pH=" + String(ph,2) + " exceeds safe limit!";
    Serial2.print(alarm); Serial2.write('\n'); Serial2.flush();
  }
  if (ph < 4.0f) {
    String alarm = "ALARM,PH_CRITICAL,pH=" + String(ph,2) + " too acidic!";
    Serial2.print(alarm); Serial2.write('\n'); Serial2.flush();
  }
  if (d1 == 0) { Serial2.print("ALARM,SENSOR_ERR,A0221AU zero"); Serial2.write('\n'); Serial2.flush(); }
  if (d2 == 0) { Serial2.print("ALARM,SENSOR_ERR,HC-SR04 zero"); Serial2.write('\n'); Serial2.flush(); }
}

// =====================================================
// DISPLAY FUNCTIONS
// =====================================================
void splashScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN); tft.setTextSize(4); tft.setCursor(60,70); tft.print("VAT");
  tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
  tft.setCursor(55,125); tft.print("METALLURGICAL"); tft.setCursor(115,150); tft.print("SYSTEM");
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(1); tft.setCursor(105,185); tft.print("Initializing...");
  delay(3000);
}
void drawHeader() {
  tft.fillRect(0,0,SCREEN_W,28,ILI9341_NAVY);
  tft.drawLine(0,28,SCREEN_W,28,ILI9341_CYAN);
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(2); tft.setCursor(62,6); tft.print("VAT SYSTEM");
}
void drawTank(int x,int y,float waterLevel,float maxH,const char* name,bool forceRedraw) {
  int level = map((int)(waterLevel*10),0,(int)(maxH*10),0,96);
  level = constrain(level,0,96);
  if(forceRedraw){
    tft.drawRect(x,y,40,100,ILI9341_WHITE);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(2); tft.setCursor(x+4,y-20); tft.print(name);
  }
  tft.fillRect(x+2,y+2,36,96,ILI9341_BLACK);
  tft.fillRect(x+2,y+98-level,36,level,ILI9341_CYAN);
  for(int i=0;i<36;i+=12) tft.fillCircle(x+5+i,y+98-level+(waveMotion%3),2,ILI9341_WHITE);
  tft.fillRect(x-4,y+102,52,10,ILI9341_BLACK);
  tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(1); tft.setCursor(x-2,y+103); tft.print(waterLevel,1); tft.print("cm");
}
void drawPanelT1(int x,int y,float rawDist,float waterLevel,bool isShutdown){
  tft.fillRect(x,y,100,70,ILI9341_BLACK); tft.drawRect(x,y,100,70,isShutdown?ILI9341_RED:ILI9341_CYAN);
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(1); tft.setCursor(x+4,y+4); tft.print("UPPER TANK");
  tft.setTextColor(ILI9341_YELLOW); tft.setCursor(x+4,y+17); tft.print("Dist: "); tft.print(rawDist,2); tft.print("cm");
  tft.setCursor(x+4,y+29); tft.print("Level:"); tft.print(waterLevel,1); tft.print("cm");
  if(isShutdown){
    tft.setTextColor(ILI9341_RED); tft.setCursor(x+4,y+42); tft.print("PUMP: OFF"); tft.setCursor(x+4,y+54); tft.print("TANK FULL");
  } else {
    tft.setTextColor(ILI9341_GREEN); tft.setCursor(x+4,y+42); tft.print("PUMP: ON");
  }
}
void drawPanelT2(int x,int y,float rawDist,float waterLevel,float volume,float limeMass){
  tft.fillRect(x,y,100,80,ILI9341_BLACK); tft.drawRect(x,y,100,80,ILI9341_CYAN);
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(1); tft.setCursor(x+4,y+4); tft.print("LOWER TANK");
  tft.setTextColor(ILI9341_YELLOW); tft.setCursor(x+4,y+17); tft.print("Dist: "); tft.print(rawDist,2); tft.print("cm");
  tft.setCursor(x+4,y+29); tft.print("Level:"); tft.print(waterLevel,1); tft.print("cm");
  tft.setTextColor(ILI9341_CYAN); tft.setCursor(x+4,y+41); tft.print("Vol:  "); tft.print(volume,2); tft.print("L");
  tft.setTextColor(ILI9341_GREEN); tft.setCursor(x+4,y+53); tft.print("Lime: "); tft.print(limeMass,1); tft.print("g");
}
void drawGauge(int cx,int cy,int r,float value,float maxVal,uint16_t color,const char* label){
  tft.drawCircle(cx,cy,r,ILI9341_WHITE); tft.fillCircle(cx,cy,r-2,ILI9341_BLACK);
  float angle=(value/maxVal)*180.0f; int px=cx+(int)(cos(radians(angle-180))*(r-2)); int py=cy+(int)(sin(radians(angle-180))*(r-2));
  tft.drawLine(cx,cy,px,py,color);
  tft.fillRect(cx-22,cy+r+2,48,18,ILI9341_BLACK);
  tft.setTextColor(color); tft.setTextSize(1); tft.setCursor(cx-18,cy+r+4); tft.print(label);
  tft.setCursor(cx-18,cy+r+13); tft.print(value,2);
}
void drawStatusBar(){
  tft.fillRect(0,210,SCREEN_W,30,ILI9341_BLACK); tft.drawLine(0,210,SCREEN_W,210,ILI9341_CYAN);
  if(pumpShutdown) tft.setTextColor(ILI9341_RED);
  else if(blink) tft.setTextColor(ILI9341_GREEN);
  else tft.setTextColor(ILI9341_DARKGREY);
  tft.setTextSize(1); tft.setCursor(5,220); tft.print(pumpShutdown?"PUMP:OFF":"PUMP:ON ");
  tft.setTextColor((ph<6.5f||ph>8.5f)?ILI9341_RED:ILI9341_GREEN); tft.setCursor(80,220); tft.print("pH:"); tft.print(ph,2);
  tft.setTextColor(ILI9341_YELLOW); tft.setCursor(155,220); tft.print("D1:"); tft.print(d1,1);
  tft.setCursor(240,220); tft.print("D2:"); tft.print(d2,1);
}
void updateUI(float wl1,float wl2,float vol2,float lime){
  drawTank(8,38,wl1,TANK1_HEIGHT,"T1",firstDraw);
  drawTank(57,38,wl2,TANK2_MAX_HEIGHT,"T2",firstDraw);
  drawPanelT1(105,36,d1,wl1,pumpShutdown);
  drawPanelT2(105,110,d2,wl2,vol2,lime);
  drawGauge(262,78,24,ph,14.0f,ILI9341_GREEN,"pH");
  drawStatusBar();
  firstDraw=false; waveMotion++; blink=!blink;
}

// =====================================================
// CALIBRATION COMMAND HANDLER (via Serial Monitor)
// =====================================================
void handleCalibrationCommands() {
  static bool calibMode = false;
  static float adc_ph7 = 0, adc_ph4 = 0;

  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "calib") {
    calibMode = true;
    adc_ph7 = adc_ph4 = 0;
    Serial.println("[CAL] Calibration mode entered. Place probe in pH 7 buffer, wait stable, then send 'ph7'");
    return;
  }
  if (!calibMode) return;

  if (cmd == "ph7") {
    long sum = 0;
    for (int i=0; i<10; i++) { sum += analogRead(PH_PIN); delay(50); }
    adc_ph7 = sum / 10.0f;
    Serial.print("[CAL] pH 7 ADC = "); Serial.println(adc_ph7);
    Serial.println("Now place probe in pH 4 (or 10) buffer, wait stable, then send 'ph4'");
  }
  else if (cmd == "ph4") {
    long sum = 0;
    for (int i=0; i<10; i++) { sum += analogRead(PH_PIN); delay(50); }
    adc_ph4 = sum / 10.0f;
    Serial.print("[CAL] pH 4 ADC = "); Serial.println(adc_ph4);
    if (adc_ph7 != 0 && adc_ph4 != 0) {
      float slope = (7.0 - 4.0) / (adc_ph7 - adc_ph4);
      float intercept = 7.0 - slope * adc_ph7;
      Serial.print("[CAL] Slope = "); Serial.println(slope, 6);
      Serial.print("[CAL] Intercept = "); Serial.println(intercept, 4);
      savePHCalibration(slope, intercept);
      calibMode = false;
      Serial.println("[CAL] Calibration complete. Offset will still be applied.");
    } else {
      Serial.println("[CAL] Error: missing pH 7 reading. Send 'ph7' first.");
    }
  }
  else if (cmd == "exit") {
    calibMode = false;
    Serial.println("[CAL] Calibration mode cancelled.");
  }
  else {
    Serial.println("[CAL] Commands: ph7, ph4, exit");
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH); delay(10);
  digitalWrite(TFT_RST, LOW);  delay(10);
  digitalWrite(TFT_RST, HIGH); delay(10);
  tft.begin(); tft.setRotation(1); tft.fillScreen(ILI9341_BLACK);

  Serial.begin(115200);
  Serial1.begin(9600);
  Serial2.begin(9600);

  pinMode(TRIG_HC, OUTPUT);
  pinMode(ECHO_HC, INPUT);
  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, LOW);
  digitalWrite(TRIG_HC, LOW);

  for (int i = 0; i < PH_AVG_WINDOW; i++) phBuffer[i] = 7.0;
  phSum = 7.0 * PH_AVG_WINDOW;
  phIndex = 0;
  phBufferFull = false;

  splashScreen(); tft.fillScreen(ILI9341_BLACK); drawHeader();

  loadPHCalibration();

  delay(300);
  Serial.println("=== VAT LEACHING MEGA STARTED (Lower tank empty at 22 cm, full at 0 cm) ===");
  Serial.println("Upper : A0221AU  Serial1 RX1=19 TX1=18");
  Serial.println("Lower : HC‑SR04  TRIG=28 ECHO=30 (empty=22cm, full=0cm, max water=22cm)");
  Serial.println("pH    : Offset applied to make water read 7.0 (type 'calib' for real buffers)");
  Serial.println("ESP32 : Serial2  TX2=16 RX2=17");
  Serial.println("===========================================================");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  handleCalibrationCommands();

  d1 = readUART_Ultrasonic();
  delay(30);
  d2 = readHC_SR04(TRIG_HC, ECHO_HC);
  ph = readPH();

  float wl1  = getWaterLevel_T1(d1);
  float wl2  = getWaterLevel_T2(d2);
  float vol2 = getVolume_T2(wl2);
  float lime = getLimeMass(ph, vol2);

  // Pump control based on upper tank distance
  if (d1 > 0 && d1 <= TANK1_SHUTDOWN_DIST) {
    pumpShutdown = true;
    digitalWrite(RELAY, HIGH);
  } else if (d1 >= TANK1_START_DIST || d1 == 0) {
    pumpShutdown = false;
    digitalWrite(RELAY, LOW);
  }

  Serial.print("D1="); Serial.print(d1,2);
  Serial.print(" D2="); Serial.print(d2,2);
  Serial.print(" wl2="); Serial.print(wl2,2);
  Serial.print(" pH="); Serial.println(ph,2);

  updateUI(wl1, wl2, vol2, lime);
  sendToESP32(wl1, wl2, vol2, lime);

  delay(800);
}