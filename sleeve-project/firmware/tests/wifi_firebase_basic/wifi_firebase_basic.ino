/*
  WiFi + Firebase basic test - XIAO ESP32-S3 (no sensors needed)
  Connects to WiFi, then every 10 seconds sends a made-up reading to a Firebase Realtime Database
  and prints the server's answer. Use it to prove WiFi, the database URL and the secret all work
  before running the full sleeve_cloud firmware.

  Setup:
    1. Fill in the five settings below. (Do not share this file after you do.)
    2. Board: XIAO_ESP32S3. Tools > USB CDC On Boot: Enabled. Serial Monitor 115200 baud.
    3. In the Firebase console (Realtime Database > Data) a "test/<DEVICE_ID>" entry should appear.
  Libraries: none to install (WiFi, WiFiClientSecure and HTTPClient come with the ESP32 board package).
  Result codes printed: 200 = success, 401 = wrong secret or rules, 404 = wrong URL, negative = network problem.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

const char* WIFI_SSID = "YOUR_WIFI_NAME";        // 2.4 GHz network only
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* DB_URL    = "https://YOUR-PROJECT-default-rtdb.firebaseio.com";   // from the Firebase console, no trailing /
const char* DB_SECRET = "YOUR_DATABASE_SECRET";
const char* DEVICE_ID = "joe";                   // letters and numbers only

void setup() {
  Serial.begin(115200);
  delay(1500);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi lost, waiting..."); delay(2000); return; }

  // made-up reading: a number that wanders so you can see it change in the console
  float fake = 90.0f + 5.0f * sin(millis() / 10000.0f);
  String body = String("{\"t\":{\".sv\":\"timestamp\"},\"value\":") + String(fake, 1) + "}";

  WiFiClientSecure client;
  client.setInsecure();                          // skips certificate checking; fine for a prototype on your own WiFi
  HTTPClient http;
  String url = String(DB_URL) + "/test/" + DEVICE_ID + ".json?auth=" + DB_SECRET;   // PUT overwrites this one entry
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(body);
    Serial.printf("Sent %.1f -> result %d  %s\n", fake, code, code == 200 ? "(OK)" : http.getString().c_str());
    http.end();
  } else {
    Serial.println("Could not start the request (check DB_URL)");
  }
  delay(10000);
}
