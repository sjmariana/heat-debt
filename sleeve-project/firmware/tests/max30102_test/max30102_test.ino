/*
  MAX30102 only - diagnostic v2 for the XIAO ESP32-S3
  Once per second prints: how many samples the sensor produced, the latest red / IR values,
  and the chip's own die temperature (proves the chip is alive and the I2C link is good).
  Library: SparkFun MAX3010x Pulse and Proximity Sensor Library
  Serial Monitor: 115200 baud (Tools > USB CDC On Boot: Enabled)

  Try it three ways: nothing on the sensor, a fingertip resting lightly on the window,
  and the window covered with your palm.
*/
#include <Wire.h>
#include "MAX30105.h"

MAX30105 sensor;
unsigned long lastPrint = 0;
unsigned long samples = 0;
uint32_t lastRed = 0, lastIR = 0;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin();                 // D4 = SDA, D5 = SCL
  Wire.setClock(100000);

  if (!sensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("begin() FAILED - sensor not answering");
    while (true) delay(1000);
  }
  Serial.printf("Part ID: 0x%02X (want 0x15)   Revision: 0x%02X\n", sensor.readPartID(), sensor.getRevisionID());

  sensor.softReset();
  delay(100);
  // powerLevel (LED current), sampleAverage, ledMode (2 = red+IR), sampleRate, pulseWidth, adcRange
  sensor.setup(0x3F, 4, 2, 200, 411, 16384);
  sensor.clearFIFO();
  sensor.enableDIETEMPRDY();    // needed for the die-temperature read
  Serial.println("Running. Each line: samples in last second | last red | last IR | die temp");
  lastPrint = millis();
}

void loop() {
  sensor.check();
  while (sensor.available()) {
    lastRed = sensor.getFIFORed();
    lastIR  = sensor.getFIFOIR();
    sensor.nextSample();
    samples++;
  }

  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    float dieC = sensor.readTemperature();
    Serial.printf("samples/s: %lu | red: %lu | ir: %lu | die temp: %.2f C\n", samples, lastRed, lastIR, dieC);
    samples = 0;
  }
  delay(2);
}
