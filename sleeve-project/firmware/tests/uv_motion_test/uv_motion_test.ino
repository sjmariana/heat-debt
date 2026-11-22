/*
  LTR390 (UV) + LIS3DSH (motion) bench test - XIAO ESP32-S3
  Wiring (both share the I2C bus with the other sensors): SDA = D4, SCL = D5, 3V3, GND.
    LTR390 : address 0x53.
    Accelerometer: works with LIS3DH (0x18/0x19) or LIS3DSH (0x1E/0x1D); the sketch finds which.
             CS tied to 3V3 (selects I2C). SDO/SA0 to GND or 3V3 picks the address. Leave INT/ADC pins open.
  Library: Adafruit LTR390 (install with Adafruit BusIO when asked).
  Serial Monitor: 115200 baud (Tools > USB CDC On Boot: Enabled).
  Try: point the LTR390 at a window or outdoor sun (UV index will read near 0 indoors), and shake the LIS3DSH.
*/
#include <Wire.h>
#include "Adafruit_LTR390.h"

Adafruit_LTR390 ltr;
bool ltrOk = false, accOk = false;
uint8_t accAddr = 0;

bool isH3 = false;                       // true = LIS3DH (WHO_AM_I 0x33), false = LIS3DSH (0x3F)
bool accWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(accAddr); Wire.write(reg); Wire.write(v);
  return Wire.endTransmission() == 0;
}
bool accRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  if (isH3 && n > 1) reg |= 0x80;                       // LIS3DH needs this bit for multi-byte reads
  Wire.beginTransmission(accAddr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;      // repeated start
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


void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin();
  Wire.setClock(100000);

  Serial.print("Devices on the bus:");
  for (uint8_t a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a); }
  Serial.println();

  ltrOk = ltr.begin();
  Serial.println(ltrOk ? "LTR390 ok (0x53)" : "LTR390 NOT found");
  if (ltrOk) {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_18);
    ltr.setResolution(LTR390_RESOLUTION_16BIT);
  }

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
}

void loop() {
  if (ltrOk && ltr.newDataAvailable()) {
    uint32_t raw = ltr.readUVS();
    // 16-bit = 25 ms integration, gain 18x. Datasheet: 2300 counts per UV index at 18x / 400 ms, scales with time.
    float uvi = raw / (2300.0f * (25.0f / 400.0f));
    Serial.printf("UV raw: %lu  UVI ~ %.2f   ", (unsigned long)raw, uvi);
  }
  if (accOk) {
    float gx, gy, gz;
    if (readAccelG(gx, gy, gz)) {
      Serial.printf("Accel g: x %.2f  y %.2f  z %.2f  |a| %.2f", gx, gy, gz, sqrtf(gx*gx + gy*gy + gz*gz));
    } else Serial.print("accel read failed");
  }
  Serial.println();
  delay(500);
}
