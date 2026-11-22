/*
  SHT45 only - detailed diagnostic sketch for the XIAO ESP32-S3
  Scans the bus, then reports every step of each measurement so we can see where it fails.
  No extra libraries needed. Serial Monitor: 115200 baud (Tools > USB CDC On Boot: Enabled).

  I2C status codes from endTransmission():
    0 = ok, 2 = no answer at the address, 3 = data byte not acknowledged, 4 = other error, 5 = timeout
*/
#include <Wire.h>

uint8_t addr = 0x44;            // changed automatically to 0x45 if that is what answers

uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

int sendCmd(uint8_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write(cmd);
  return Wire.endTransmission();
}

void scanBus() {
  Serial.print("Devices on the bus:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.printf("  (%d found)\n", n);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin();                 // D4 = SDA, D5 = SCL
  Wire.setClock(100000);
  Wire.setTimeOut(100);

  scanBus();

  // does 0x44 or 0x45 answer?
  Wire.beginTransmission(0x44);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(0x45);
    if (Wire.endTransmission() == 0) addr = 0x45;
  }
  Serial.printf("Using address 0x%02X\n", addr);

  // soft reset, then read the serial number (proves the chip speaks the SHT4x protocol)
  Serial.printf("soft reset status: %d\n", sendCmd(0x94));
  delay(10);
  int st = sendCmd(0x89);
  Serial.printf("serial-number command status: %d\n", st);
  delay(5);
  int got = Wire.requestFrom(addr, (uint8_t)6);
  Serial.printf("serial-number bytes received: %d (want 6)\n", got);
  if (got == 6) {
    uint8_t s[6]; for (int i = 0; i < 6; i++) s[i] = Wire.read();
    Serial.printf("serial number: %02X %02X %02X %02X %02X %02X  CRC %s / %s\n",
                  s[0], s[1], s[2], s[3], s[4], s[5],
                  crc8(s, 2) == s[2] ? "ok" : "BAD", crc8(s + 3, 2) == s[5] ? "ok" : "BAD");
  }
  Serial.println("----");
}

void loop() {
  int st = sendCmd(0xFD);                   // measure, high precision
  delay(25);                                // far longer than the sensor needs
  int got = Wire.requestFrom(addr, (uint8_t)6);

  if (got != 6) {
    Serial.printf("measure cmd status: %d | bytes received: %d (want 6)  -> READ FAILED\n", st, got);
    delay(2000);
    return;
  }
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  bool crcT = crc8(b, 2) == b[2];
  bool crcH = crc8(b + 3, 2) == b[5];

  uint16_t rawT = (b[0] << 8) | b[1];
  uint16_t rawH = (b[3] << 8) | b[4];
  float tC = -45.0f + 175.0f * rawT / 65535.0f;
  float rh = -6.0f + 125.0f * rawH / 65535.0f;

  Serial.printf("raw: %02X %02X %02X | %02X %02X %02X   CRC T:%s H:%s   ",
                b[0], b[1], b[2], b[3], b[4], b[5], crcT ? "ok" : "BAD", crcH ? "ok" : "BAD");
  Serial.printf("Temp: %.2f C (%.2f F)   Humidity: %.1f %%\n", tC, tC * 9.0f / 5.0f + 32.0f, rh);
  delay(2000);
}
