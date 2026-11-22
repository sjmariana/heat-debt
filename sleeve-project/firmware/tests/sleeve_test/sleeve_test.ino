/*
  Sleeve hub bench test - XIAO ESP32-S3
  Reads: TMP119 (skin temp), SHT45 (air temp + humidity), MAX30102 (heart rate)
  Shows values on the 0.96" OLED and prints them to the Serial Monitor.
  Temperatures are shown in deg F. Blinks the LED on D2 (and the board's built-in LED) when skin temp is above ALERT_TEMP_F.

  Wiring: SDA = D4, SCL = D5, LED + resistor on D2, MAX30102 INT on D0 (not used in this sketch).

  Libraries to install (Sketch > Include Library > Manage Libraries):
    - Adafruit SSD1306
    - Adafruit GFX Library
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
  Board: XIAO_ESP32S3. Tools > USB CDC On Boot: Enabled (so Serial Monitor works). Baud 115200.

  TMP119 and SHT45 are read directly over I2C, so no extra libraries are needed for them.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"      // SparkFun library; works for the MAX30102
#include "heartRate.h"

// ---------- settings you can change ----------
const float ALERT_TEMP_F = 98.6;    // LED turns on above this skin temperature (deg F)
const float ALERT_CLEAR_F = 98.1;   // ...and turns off below this (prevents flicker)
const long  FINGER_MIN_IR = 50000;  // below this, the MAX30102 thinks nothing is touching it
// ---------------------------------------------

const uint8_t ADDR_OLED   = 0x3C;
const uint8_t ADDR_SHT45  = 0x44;
const uint8_t ADDR_TMP119 = 0x48;
const int LED_PIN = D2;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
MAX30105 ppg;

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
  display.display();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
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

  // ----- alert logic with hysteresis -----
  if (!isnan(skinC)) {
    float skinF = toF(skinC);
    if (!alertOn && skinF > ALERT_TEMP_F) alertOn = true;
    else if (alertOn && skinF < ALERT_CLEAR_F) alertOn = false;
  }
  if (alertOn) setLed((now / 300) % 2 == 0);   // blink fast
  else         setLed(false);

  // ----- display and serial -----
  if (now - lastDraw >= 500) { lastDraw = now; drawScreen(); }
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    Serial.printf("HR: %.0f bpm | Skin: %.2f F | Air: %.2f F | Hum: %.1f %% | Alert: %s",
                  bpm, toF(skinC), toF(airC), humRH, alertOn ? "YES" : "no");
    if (shtFails) Serial.printf(" | SHT45 fails: %lu (last: %s)", shtFails, shtErr == 1 ? "no data" : "CRC");
    Serial.println();
  }
}
