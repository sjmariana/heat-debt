/*
  SHT45 live view + heater self-test - XIAO ESP32-S3
  1. Takes a reading 4 times a second so even a short breath shows up.
  2. 8 seconds after start (and any time you type h + Enter in the Serial Monitor) it fires the
     SHT45's built-in heater for 1 second. That changes temperature and humidity on purpose,
     so it proves the sensor and the code respond, with no breathing needed.

  Wiring: SDA = D4, SCL = D5, power 3V3, GND. No extra libraries.
  Serial Monitor: 115200 baud (Tools > USB CDC On Boot: Enabled).
  Do not leave the heater firing continuously; this sketch only fires it on request.
*/
#include <Wire.h>

const uint8_t ADDR = 0x44;

uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

// Sends a command, waits, reads 6 bytes. cmd 0xFD = measure (high precision), 0x39 = heater 200 mW for 1 s then measure.
bool measure(uint8_t cmd, uint16_t waitMs, float &tC, float &rh) {
  Wire.beginTransmission(ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;
  delay(waitMs);
  if (Wire.requestFrom(ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  uint16_t rawT = (b[0] << 8) | b[1];
  uint16_t rawH = (b[3] << 8) | b[4];
  tC = -45.0f + 175.0f * rawT / 65535.0f;
  rh = -6.0f + 125.0f * rawH / 65535.0f;
  return true;
}

float baseT = NAN, baseH = NAN;
float maxH = -1000, minH = 1000;
int nBase = 0;
unsigned long autoAt = 8000;
bool autoDone = false;

void heaterTest() {
  float t, h;
  Serial.println(">>> HEATER TEST: firing 1 s heater pulse...");
  if (measure(0x39, 1100, t, h)) {
    Serial.printf(">>> right after heating: %.1f F (%.1f C), humidity %.1f %%   (baseline was %.1f F, %.1f %%)\n",
                  t * 9 / 5 + 32, t, h, baseT * 9 / 5 + 32, baseH);
    Serial.println(">>> Expect temperature UP by several degrees and humidity DOWN noticeably. Watch it recover below.");
  } else {
    Serial.println(">>> heater test: read failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin();
  Wire.setClock(100000);
  Serial.println("SHT45 live view. Breathe on the chip, or wait for the automatic heater test at 8 s.");
  Serial.println("Type h + Enter any time to fire the heater test again.");
}

void loop() {
  static unsigned long last = 0;
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'h' || c == 'H') heaterTest();
  }
  if (!autoDone && millis() > autoAt && nBase >= 8) { autoDone = true; heaterTest(); }

  if (millis() - last < 250) return;
  last = millis();

  float t, h;
  if (!measure(0xFD, 12, t, h)) { Serial.println("read failed"); return; }

  if (nBase < 8) {                       // average the first 8 readings as the baseline
    baseT = isnan(baseT) ? t : baseT + t; baseH = isnan(baseH) ? h : baseH + h;
    if (++nBase == 8) { baseT /= 8; baseH /= 8; }
  }
  if (h > maxH) maxH = h;
  if (h < minH) minH = h;

  int bars = constrain((int)(h / 2.5f), 0, 40);
  char bar[42]; for (int i = 0; i < bars; i++) bar[i] = '#'; bar[bars] = 0;
  Serial.printf("%.1f F  %5.1f %%  (min %.1f / max %.1f)  %s\n", t * 9 / 5 + 32, h, minH, maxH, bar);
}
