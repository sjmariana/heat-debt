// Copy this file to config.h and fill in your values. config.h is git-ignored so secrets stay private.
#pragma once

// ---- alert behaviour ----
const float ALERT_TEMP_F  = 98.6;    // LED + haptic turn on above this skin temperature (deg F)
const float ALERT_CLEAR_F = 98.1;    // ...and turn off below this (prevents flicker)
const long  FINGER_MIN_IR = 50000;   // below this the MAX30102 thinks nothing is touching it

// ---- WiFi + Firebase ----
const char* WIFI_SSID   = "YOUR_WIFI_NAME";        // 2.4 GHz only
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
const char* DB_URL      = "https://YOUR-PROJECT-default-rtdb.firebaseio.com";  // no trailing slash
const char* DB_SECRET   = "YOUR_DATABASE_SECRET";  // lets the device write. Never put this in the dashboard.
const char* DEVICE_ID   = "joe";                   // letters/numbers only; same ID goes in the dashboard Profile
const int   UPLOAD_EVERY_S = 15;
