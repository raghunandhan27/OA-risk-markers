/*
  ===========================================================================
  Osteoarthritis Monitoring Prototype — SENSOR NODE (ESP32)
  ===========================================================================
  This version does NOT do any filtering or hosting itself. Its only job is:
    1. Read both MPU6050s, the FSR, and the piezo.
    2. Package the RAW values into JSON.
    3. HTTP POST them to a Python backend running on your PC/laptop/Raspberry Pi.

  All filtering (complementary filter, moving average, crepitus detection)
  and the risk score now live in backend.py — see that file. This split makes
  it much easier to later swap in a real trained ML model, since Python has
  far better libraries for that than the ESP32 does.

  LIBRARIES TO INSTALL (Sketch -> Include Library -> Manage Libraries)
  ----------------------------------------------------------------------
  1. "Adafruit MPU6050" by Adafruit
  2. "Adafruit Unified Sensor" (auto-installed as dependency)
  3. "ArduinoJson" by Benoit Blanchon (v6.x)
  Board package: "esp32 by Espressif Systems"
  ===========================================================================
*/

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// 1. USER CONFIG — EDIT THESE
// ---------------------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Your PC's local IP address and the port backend.py listens on (default 5000).
// Find your PC's IP with `ipconfig` (Windows) or `ifconfig` / `ip addr` (Mac/Linux).
const char* SERVER_URL = "http://192.168.1.100:5000/ingest";

// ---------------------------------------------------------------------------
// 2. PIN MAP (identical wiring to the all-in-one version — see README.md)
// ---------------------------------------------------------------------------
#define I2C_SDA_PIN   21
#define I2C_SCL_PIN   22
#define FSR_PIN       34
#define PIEZO_PIN     35
#define MPU_THIGH_ADDR 0x68   // AD0 -> GND
#define MPU_SHANK_ADDR 0x69   // AD0 -> 3.3V

Adafruit_MPU6050 mpuThigh;
Adafruit_MPU6050 mpuShank;

const unsigned long SAMPLE_INTERVAL_MS = 50; // 20 Hz — good balance of responsiveness vs WiFi/HTTP overhead
unsigned long lastSampleTime = 0;

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  if (!mpuThigh.begin(MPU_THIGH_ADDR, &Wire)) {
    Serial.println("!! Thigh MPU6050 not found. Check wiring/AD0.");
  }
  if (!mpuShank.begin(MPU_SHANK_ADDR, &Wire)) {
    Serial.println("!! Shank MPU6050 not found. Check wiring/AD0.");
  }
  mpuThigh.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpuThigh.setGyroRange(MPU6050_RANGE_500_DEG);
  mpuThigh.setFilterBandwidth(MPU6050_BAND_21_HZ);
  mpuShank.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpuShank.setGyroRange(MPU6050_RANGE_500_DEG);
  mpuShank.setFilterBandwidth(MPU6050_BAND_21_HZ);

  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Streaming to: ");
  Serial.println(SERVER_URL);
}

void loop() {
  unsigned long now = millis();
  if (now - lastSampleTime < SAMPLE_INTERVAL_MS) return;
  lastSampleTime = now;

  sensors_event_t aT, gT, tT, aS, gS, tS;
  mpuThigh.getEvent(&aT, &gT, &tT);
  mpuShank.getEvent(&aS, &gS, &tS);
  int fsrRaw   = analogRead(FSR_PIN);
  int piezoRaw = analogRead(PIEZO_PIN);

  StaticJsonDocument<512> doc;
  doc["t"] = now;                       // ms since boot — backend uses this for dt
  doc["accThighY"] = aT.acceleration.y;
  doc["accThighZ"] = aT.acceleration.z;
  doc["gyroThighX"] = gT.gyro.x;        // rad/s
  doc["accShankY"] = aS.acceleration.y;
  doc["accShankZ"] = aS.acceleration.z;
  doc["gyroShankX"] = gS.gyro.x;        // rad/s
  doc["fsrRaw"] = fsrRaw;
  doc["piezoRaw"] = piezoRaw;

  String payload;
  serializeJson(doc, payload);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(200); // don't let a slow/missing server stall sampling for too long
    int code = http.POST(payload);
    if (code <= 0) {
      Serial.print("POST failed: ");
      Serial.println(http.errorToString(code));
    }
    http.end();
  } else {
    Serial.println("WiFi disconnected, skipping send.");
  }
}
