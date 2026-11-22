/*
  Sleeve hub CLOUD version - XIAO ESP32-S3 (UNTESTED on hardware: first run, watch the Serial Monitor)
  Same sensing as sleeve_test, plus: every UPLOAD_EVERY_S seconds it sends HR / skin / air / humidity
  to a Firebase Realtime Database over WiFi. Uploading runs in its own task on core 0 so it never
  stalls the heart-rate sampling on core 1.
  Reads: TMP119 (skin temp), SHT45 (air temp + humidity), MAX30102 (heart rate)
  Shows values on the 0.96" OLED and prints them to the Serial Monitor.
  Temperatures are shown in deg F. Blinks the LED on D2 (and the board's built-in LED) when skin temp is above ALERT_TEMP_F.

  Wiring: SDA = D4, SCL = D5, LED + resistor on D2, MAX30102 INT on D0 (not used in this sketch).

  Libraries to install (Sketch > Include Library > Manage Libraries):
    - Adafruit SSD1306
    - Adafruit GFX Library
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
    - Adafruit LTR390 (accept the Adafruit BusIO dependency)
  Board: XIAO_ESP32S3. Tools > USB CDC On Boot: Enabled (so Serial Monitor works). Baud 115200.

  TMP119 and SHT45 are read directly over I2C, so no extra libraries are needed for them.
*/

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_LTR390.h"   // UV sensor (install "Adafruit LTR390")
#include "MAX30105.h"      // SparkFun library; works for the MAX30102
#include "heartRate.h"

#include "config.h"   // your WiFi, Firebase and alert settings (copy config.example.h to config.h)

const uint8_t ADDR_OLED   = 0x3C;
const uint8_t ADDR_SHT45  = 0x44;
const uint8_t ADDR_TMP119 = 0x48;
const int LED_PIN = D2;
const int HAPTIC_PIN = D3;           // drives the vibration motor through a transistor (never directly)
const bool HAPTIC_ENABLED = true;    // set false to silence the motor
const unsigned long BUZZ_ON_MS = 250, BUZZ_PERIOD_MS = 1500;   // pulse pattern while the alert is on

Adafruit_SSD1306 display(128, 64, &Wire, -1);
MAX30105 ppg;
Adafruit_LTR390 ltr;

// ---- LTR390 (UV) and LIS3DH/LIS3DSH (motion) ----
bool ltrOk = false, accOk = false;
uint8_t accAddr = 0;
float uvi = NAN;                       // UV index
float activityMg = 0, peakG = 1.0f;    // movement intensity (milli-g) and biggest jolt over the last second
float accSumDev = 0, accPeak = 1.0f; int accN = 0;
unsigned long lastAcc = 0, lastUv = 0;

bool isH3 = false;                       // true = LIS3DH (WHO_AM_I 0x33), false = LIS3DSH (0x3F)
bool accWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(accAddr); Wire.write(reg); Wire.write(v);
  return Wire.endTransmission() == 0;
}
bool accRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  if (isH3 && n > 1) reg |= 0x80;                       // LIS3DH needs this bit for multi-byte reads
  Wire.beginTransmission(accAddr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(accAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool readAccelG(float &gx, float &gy, float &gz) {
  uint8_t b[6];
  if (!accRead(0x28, b, 6)) return false;
  int16_t x = (b[1] << 8) | b[0], y = (b[3] << 8) | b[2], z = (b[5] << 8) | b[4];
  float k = isH3 ? 0.002f / 16.0f : 0.00012f;        // LIS3DH: 2 mg per 12-bit step (value is left-aligned); LIS3DSH: 0.12 mg
  gx = x * k; gy = y * k; gz = z * k;
  return true;
}


bool oledOk = false, ppgOk = false;

// latest readings
float skinC = NAN, airC = NAN, humRH = NAN, bpm = 0;
bool  alertOn = false;

// heart rate averaging
const byte RATE_SIZE = 4;
float rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

// SHT45 non-blocking state
int shtErr = 0;                 // last error code: 1 = no data, 2 = CRC failed
unsigned long shtFails = 0;     // how many reads have failed since power-up
bool shtWaiting = false;
unsigned long shtStart = 0;

unsigned long lastTemp = 0, lastDraw = 0, lastPrint = 0;

// ---------- helpers ----------
float toF(float c) { return c * 9.0f / 5.0f + 32.0f; }   // sensors report deg C; convert for display

uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

bool readTMP119(float &t) {
  Wire.beginTransmission(ADDR_TMP119);
  Wire.write(0x00);                       // temperature register
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(ADDR_TMP119, (uint8_t)2) != 2) return false;
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  if (raw == 0x8000) return false;        // no valid conversion yet
  t = (int16_t)raw * 0.0078125f;          // 7.8125 m°C per step
  return true;
}

void startSHT45() {
  Wire.beginTransmission(ADDR_SHT45);
  Wire.write(0xFD);                       // measure, high precision
  Wire.endTransmission();
  shtStart = millis();
  shtWaiting = true;
}

// returns 0 = ok, 1 = sensor did not return 6 bytes, 2 = CRC check failed
int readSHT45(float &t, float &rh) {
  if (Wire.requestFrom(ADDR_SHT45, (uint8_t)6) != 6) return 1;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return 2;
  uint16_t rawT = (b[0] << 8) | b[1];
  uint16_t rawH = (b[3] << 8) | b[4];
  t = -45.0f + 175.0f * rawT / 65535.0f;
  rh = -6.0f + 125.0f * rawH / 65535.0f;
  rh = constrain(rh, 0.0f, 100.0f);
  return 0;
}

// ---------- cloud upload (runs on core 0) ----------
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
struct Snapshot { float bpm, skinF, airF, rh, uvi, act, peak; bool alert; } snap = {0, NAN, NAN, NAN, NAN, 0, 1, false};
volatile bool wifiUp = false, lastUploadOk = false;
volatile unsigned long uploadsOk = 0, uploadsFailed = 0;

void publishSnapshot() {                 // called from loop() once a second
  portENTER_CRITICAL(&dataMux);
  snap.bpm = bpm; snap.skinF = isnan(skinC) ? NAN : toF(skinC);
  snap.airF = isnan(airC) ? NAN : toF(airC); snap.rh = humRH; snap.alert = alertOn;
  snap.uvi = uvi; snap.act = activityMg; snap.peak = peakG;
  portEXIT_CRITICAL(&dataMux);
}

String buildJson(const Snapshot &s, bool withServerTime) {
  String j = "{";
  if (withServerTime) j += "\"t\":{\".sv\":\"timestamp\"},";
  j += "\"hr\":" + String((int)s.bpm);
  if (!isnan(s.skinF)) j += ",\"skin\":" + String(s.skinF, 1);
  if (!isnan(s.airF))  j += ",\"air\":"  + String(s.airF, 1);
  if (!isnan(s.rh))    j += ",\"rh\":"   + String(s.rh, 1);
  if (!isnan(s.uvi))   j += ",\"uv\":"   + String(s.uvi, 2);
  j += ",\"act\":" + String((int)s.act) + ",\"pk\":" + String(s.peak, 2);
  j += String(",\"alert\":") + (s.alert ? "true" : "false") + "}";
  return j;
}

bool sendJson(const char* method, const String &path, const String &body) {
  WiFiClientSecure client;
  client.setInsecure();                  // prototype: skips certificate check (fine on your own WiFi)
  HTTPClient http;
  String url = String(DB_URL) + path + ".json?auth=" + DB_SECRET;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.sendRequest(method, body);
  http.end();
  Serial.printf("[cloud] %s %s -> %d\n", method, path.c_str(), code);
  return code == 200;
}

void uploadTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (;;) {
    wifiUp = (WiFi.status() == WL_CONNECTED);
    if (wifiUp) {
      Snapshot s;
      portENTER_CRITICAL(&dataMux); s = snap; portEXIT_CRITICAL(&dataMux);
      bool ok = false;
      if (!isnan(s.skinF) || !isnan(s.airF)) {            // skip until at least one sensor has data
        bool a = sendJson("POST", String("/readings/") + DEVICE_ID, buildJson(s, true));   // history
        bool b = sendJson("PUT",  String("/latest/")   + DEVICE_ID, buildJson(s, true));   // newest only
        ok = a && b;
        if (ok) uploadsOk++; else uploadsFailed++;
      }
      lastUploadOk = ok;
    } else {
      Serial.println("[cloud] waiting for WiFi...");
    }
    vTaskDelay(pdMS_TO_TICKS(UPLOAD_EVERY_S * 1000));
  }
}

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);   // built-in LED is active-low
}

void drawScreen() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  if (bpm > 0) { display.print((int)bpm); display.setTextSize(1); display.print(" bpm"); }
  else         { display.print("--"); display.setTextSize(1); display.print(" bpm"); }

  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print("Skin: ");
  if (isnan(skinC)) display.print("--"); else { display.print(toF(skinC), 1); display.print(" F"); }

  display.setCursor(0, 38);
  display.print("Air:  ");
  if (isnan(airC)) display.print("--"); else { display.print(toF(airC), 1); display.print(" F"); }

  display.setCursor(0, 52);
  display.print("Hum:  ");
  if (isnan(humRH)) display.print("--"); else { display.print(humRH, 0); display.print(" %"); }

  if (alertOn) {                          // inverted "HIGH" tag in the corner
    display.fillRect(88, 0, 40, 14, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(94, 3);
    display.print("HIGH");
  }
  display.setTextSize(1);
  display.setCursor(100, 56);                       // tiny cloud status, bottom-right
  display.print(!wifiUp ? "w?" : (lastUploadOk ? "up" : "..."));
  display.display();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);
  setLed(false);

  Wire.begin();                           // D4 = SDA, D5 = SCL
  Wire.setClock(100000);                  // slow I2C is far more forgiving of long jumper wires

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, ADDR_OLED);
  Serial.println(oledOk ? "OLED ok" : "OLED NOT found (0x3C)");
  if (oledOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Sleeve hub starting");
    display.display();
  }

  ppgOk = ppg.begin(Wire, I2C_SPEED_STANDARD);
  Serial.println(ppgOk ? "MAX30102 ok" : "MAX30102 NOT found (0x57)");
  if (ppgOk) {
    ppg.setup();                          // default settings work for beat detection
    ppg.setPulseAmplitudeRed(0x1F);       // red LED on as a "ready" indicator
    ppg.setPulseAmplitudeGreen(0);
  }

  float t;
  Serial.println(readTMP119(t) ? "TMP119 ok" : "TMP119 NOT found (0x48)");
  startSHT45();

  ltrOk = ltr.begin();
  Serial.println(ltrOk ? "LTR390 ok" : "LTR390 NOT found (0x53)");
  if (ltrOk) { ltr.setMode(LTR390_MODE_UVS); ltr.setGain(LTR390_GAIN_18); ltr.setResolution(LTR390_RESOLUTION_16BIT); }

  for (uint8_t a : {0x18, 0x19, 0x1E, 0x1D}) {   // LIS3DH: 0x18/0x19. LIS3DSH: 0x1E/0x1D
    accAddr = a; uint8_t who = 0;
    isH3 = false;
    if (accRead(0x0F, &who, 1) && (who == 0x33 || who == 0x3F)) { isH3 = (who == 0x33); accOk = true; break; }
  }
  if (accOk) {
    if (isH3) { accWrite(0x20, 0x57); accWrite(0x23, 0x98); }                          // LIS3DH: 100 Hz, XYZ on; BDU, +/-4 g, high-res
    else      { accWrite(0x20, 0x6F); accWrite(0x24, 0x08); accWrite(0x25, 0x10); }    // LIS3DSH: 100 Hz, +/-4 g, auto-increment
  }
  Serial.println(accOk ? (isH3 ? "LIS3DH ok" : "LIS3DSH ok") : "Accelerometer NOT found (expect 0x18/0x19 for LIS3DH or 0x1E/0x1D for LIS3DSH)");

  xTaskCreatePinnedToCore(uploadTask, "upload", 12288, NULL, 1, NULL, 0);   // core 0
}

void loop() {
  unsigned long now = millis();

  // ----- heart rate: must run as often as possible -----
  if (ppgOk) {
    long ir = ppg.getIR();
    if (ir < FINGER_MIN_IR) {
      bpm = 0;                            // no finger
      rateSpot = 0;
      for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
      lastBeat = 0;
    } else if (checkForBeat(ir)) {
      long delta = now - lastBeat;
      lastBeat = now;
      if (delta > 300 && delta < 2000) {  // 30 to 200 bpm
        rates[rateSpot++ % RATE_SIZE] = 60000.0f / delta;
        float sum = 0; byte n = 0;
        for (byte i = 0; i < RATE_SIZE; i++) if (rates[i] > 0) { sum += rates[i]; n++; }
        if (n) bpm = sum / n;
      }
    }
    if (lastBeat && now - lastBeat > 5000) bpm = 0;   // no beat for 5 s
  }

  // ----- temperatures: once per second -----
  if (now - lastTemp >= 1000 && !shtWaiting) {
    lastTemp = now;
    float t;
    if (readTMP119(t)) skinC = t;
    startSHT45();
  }
  if (shtWaiting && now - shtStart >= 15) { // SHT45 needs up to about 9 ms
    float t, rh;
    int code = readSHT45(t, rh);
    if (code == 0) { airC = t; humRH = rh; }
    else { airC = NAN; humRH = NAN; shtErr = code; shtFails++; }
    shtWaiting = false;
  }

  // ----- motion: sample at 25 Hz, summarise once a second -----
  if (accOk && now - lastAcc >= 40) {
    lastAcc = now;
    float gx, gy, gz;
    if (readAccelG(gx, gy, gz)) {
      float mag = sqrtf(gx*gx + gy*gy + gz*gz);
      accSumDev += fabsf(mag - 1.0f); accN++;
      if (mag > accPeak) accPeak = mag;
    }
  }
  static unsigned long lastAccSum = 0;
  if (accOk && now - lastAccSum >= 1000) {
    lastAccSum = now;
    activityMg = accN ? 1000.0f * accSumDev / accN : 0;           // 0 = still; walking is roughly 100-300; hard work higher
    peakG = accPeak; accSumDev = 0; accN = 0; accPeak = 1.0f;
  }
  // ----- UV: once a second -----
  if (ltrOk && now - lastUv >= 1000 && ltr.newDataAvailable()) {
    lastUv = now;
    uvi = ltr.readUVS() / (2300.0f * (25.0f / 400.0f));          // 16-bit, 18x gain (datasheet sensitivity)
  }

  // ----- alert logic with hysteresis -----
  if (!isnan(skinC)) {
    float skinF = toF(skinC);
    if (!alertOn && skinF > ALERT_TEMP_F) alertOn = true;
    else if (alertOn && skinF < ALERT_CLEAR_F) alertOn = false;
  }
  if (alertOn) setLed((now / 300) % 2 == 0);   // blink fast
  else         setLed(false);
  // haptic: short pulses while the alert is on, silent otherwise
  digitalWrite(HAPTIC_PIN, (HAPTIC_ENABLED && alertOn && (now % BUZZ_PERIOD_MS) < BUZZ_ON_MS) ? HIGH : LOW);

  static unsigned long lastPub = 0;
  if (now - lastPub >= 1000) { lastPub = now; publishSnapshot(); }

  // ----- display and serial -----
  if (now - lastDraw >= 500) { lastDraw = now; drawScreen(); }
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    Serial.printf("HR: %.0f bpm | Skin: %.2f F | Air: %.2f F | Hum: %.1f %% | Alert: %s",
                  bpm, toF(skinC), toF(airC), humRH, alertOn ? "YES" : "no");
    Serial.printf(" | UVI: %.2f | act: %.0f mg | peak: %.2f g", uvi, activityMg, peakG);
    Serial.printf(" | WiFi: %s up:%lu/%lu", wifiUp ? "ok" : "no", uploadsOk, uploadsOk + uploadsFailed);
    if (shtFails) Serial.printf(" | SHT45 fails: %lu (last: %s)", shtFails, shtErr == 1 ? "no data" : "CRC");
    Serial.println();
  }
}
