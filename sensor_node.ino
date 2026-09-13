/*
  ============================================================
  Osteoarthritis Monitoring Prototype — ESP32 SENSOR NODE
  ============================================================
  Sensors:
    - Thigh IMU  -> MPU6500-compatible, address 0x69
    - Shank IMU  -> MPU6500-compatible, address 0x68
    - FSR        -> GPIO34
    - Piezo      -> GPIO35

  ESP32 sends RAW sensor data to Flask backend.
*/

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================================
// 1. WiFi / SERVER CONFIG
// ============================================================

const char* WIFI_SSID     = "YOUR WIFI NAME";
const char* WIFI_PASSWORD = "YOUR WIFI PASSWORD";

const char* SERVER_URL = "http://YOUR LAPTOP IP ADDRESS:5000/ingest";

// ============================================================
// 2. PIN CONFIGURATION
// ============================================================

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define FSR_PIN   34
#define PIEZO_PIN 35

// Based on the wiring we tested successfully earlier:
#define MPU_THIGH_ADDR 0x69   // Thigh AD0 -> 3.3V
#define MPU_SHANK_ADDR 0x68   // Shank AD0 -> GND

// ============================================================
// 3. MPU REGISTERS
// ============================================================

#define WHO_AM_I      0x75
#define PWR_MGMT_1    0x6B
#define CONFIG_REG    0x1A
#define GYRO_CONFIG   0x1B
#define ACCEL_CONFIG  0x1C
#define ACCEL_XOUT_H  0x3B

// ============================================================
// 4. SAMPLE TIMING
// ============================================================

const unsigned long SAMPLE_INTERVAL_MS = 100;
unsigned long lastSampleTime = 0;

// ============================================================
// 5. RAW IMU DATA STRUCTURE
// ============================================================

struct IMUData {
  float accX;
  float accY;
  float accZ;

  float gyroX;
  float gyroY;
  float gyroZ;
};

// ============================================================
// 6. WRITE MPU REGISTER
// ============================================================

void writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// ============================================================
// 7. READ MPU REGISTER
// ============================================================

uint8_t readRegister(uint8_t address, uint8_t reg) {

  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }

  Wire.requestFrom(address, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0xFF;
}

// ============================================================
// 8. INITIALIZE MPU6500
// ============================================================

bool initIMU(uint8_t address) {

  uint8_t who = readRegister(address, WHO_AM_I);

  Serial.print("IMU 0x");
  Serial.print(address, HEX);
  Serial.print(" WHO_AM_I = 0x");
  Serial.println(who, HEX);

  if (who != 0x70 && who != 0x68) {
    Serial.println("!! IMU not detected.");
    return false;
  }

  // Wake up the sensor
  writeRegister(address, PWR_MGMT_1, 0x00);
  delay(100);

  // Digital low-pass filter
  writeRegister(address, CONFIG_REG, 0x03);

  // Gyroscope: +/- 500 deg/s
  writeRegister(address, GYRO_CONFIG, 0x08);

  // Accelerometer: +/- 8g
  writeRegister(address, ACCEL_CONFIG, 0x10);

  delay(50);

  Serial.print("IMU 0x");
  Serial.print(address, HEX);
  Serial.println(" initialized OK.");

  return true;
}

// ============================================================
// 9. READ ACCEL + GYRO
// ============================================================

bool readIMU(uint8_t address, IMUData &data) {

  Wire.beginTransmission(address);
  Wire.write(ACCEL_XOUT_H);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = Wire.requestFrom(address, (uint8_t)14);

  if (received != 14) {
    return false;
  }

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  // Temperature registers — not used
  Wire.read();
  Wire.read();

  int16_t gx = (Wire.read() << 8) | Wire.read();
  int16_t gy = (Wire.read() << 8) | Wire.read();
  int16_t gz = (Wire.read() << 8) | Wire.read();

  // ----------------------------------------------------------
  // Conversion
  // Accelerometer +/-8g = 4096 LSB/g
  // Gyroscope +/-500 deg/s = 65.5 LSB/(deg/s)
  // ----------------------------------------------------------

  data.accX = ((float)ax / 4096.0) * 9.80665;
  data.accY = ((float)ay / 4096.0) * 9.80665;
  data.accZ = ((float)az / 4096.0) * 9.80665;

  // deg/s -> rad/s
  data.gyroX = ((float)gx / 65.5) * DEG_TO_RAD;
  data.gyroY = ((float)gy / 65.5) * DEG_TO_RAD;
  data.gyroZ = ((float)gz / 65.5) * DEG_TO_RAD;

  return true;
}

// ============================================================
// 10. SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" OA MONITORING SENSOR NODE");
  Serial.println(" MPU6500 + FSR + PIEZO");
  Serial.println("======================================");

  // I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  Serial.println();
  Serial.println("Checking IMUs...");

  bool thighOK = initIMU(MPU_THIGH_ADDR);
  bool shankOK = initIMU(MPU_SHANK_ADDR);

  if (!thighOK) {
    Serial.println("!! THIGH IMU ERROR");
  }

  if (!shankOK) {
    Serial.println("!! SHANK IMU ERROR");
  }

  // ADC
  analogReadResolution(12);

  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  Serial.println();
  Serial.print("Connecting to WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");

  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Streaming to: ");
  Serial.println(SERVER_URL);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" SENSOR STREAM STARTED");
  Serial.println("======================================");
}

// ============================================================
// 11. MAIN LOOP
// ============================================================

void loop() {

  unsigned long now = millis();

  if (now - lastSampleTime < SAMPLE_INTERVAL_MS) {
    return;
  }

  lastSampleTime = now;

  IMUData thigh;
  IMUData shank;

  bool thighOK = readIMU(MPU_THIGH_ADDR, thigh);
  bool shankOK = readIMU(MPU_SHANK_ADDR, shank);

  int fsrRaw = analogRead(FSR_PIN);
  int piezoRaw = analogRead(PIEZO_PIN);

  if (!thighOK || !shankOK) {

    Serial.println("!! IMU READ ERROR");

    return;
  }

  // ==========================================================
  // JSON DATA
  // ==========================================================

  StaticJsonDocument<512> doc;

  doc["t"] = now;

  // Thigh
  doc["accThighY"] = thigh.accY;
  doc["accThighZ"] = thigh.accZ;
  doc["gyroThighX"] = thigh.gyroX;

  // Shank
  doc["accShankY"] = shank.accY;
  doc["accShankZ"] = shank.accZ;
  doc["gyroShankX"] = shank.gyroX;

  // FSR + Piezo
  doc["fsrRaw"] = fsrRaw;
  doc["piezoRaw"] = piezoRaw;

  String payload;
  serializeJson(doc, payload);

  // ==========================================================
  // SEND TO FLASK BACKEND
  // ==========================================================

  if (WiFi.status() == WL_CONNECTED) {

    HTTPClient http;

    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000);

    int code = http.POST(payload);

    if (code > 0) {

      Serial.print("POST ");
      Serial.print(code);

      Serial.print(" | FSR=");
      Serial.print(fsrRaw);

      Serial.print(" | Piezo=");
      Serial.print(piezoRaw);

      Serial.print(" | Thigh Acc X=");
      Serial.print(thigh.accX, 2);

      Serial.print(" Y=");
      Serial.print(thigh.accY, 2);

      Serial.print(" Z=");
      Serial.print(thigh.accZ, 2);

      Serial.print(" | Thigh Gyro X=");
      Serial.print(thigh.gyroX, 2);

      Serial.print(" Y=");
      Serial.print(thigh.gyroY, 2);

      Serial.print(" Z=");
      Serial.print(thigh.gyroZ, 2);

      Serial.print(" | Shank Acc X=");
      Serial.print(shank.accX, 2);

      Serial.print(" Y=");
      Serial.print(shank.accY, 2);

      Serial.print(" Z=");
      Serial.print(shank.accZ, 2);

      Serial.print(" | Shank Gyro X=");
      Serial.print(shank.gyroX, 2);

      Serial.print(" Y=");
      Serial.print(shank.gyroY, 2);

      Serial.print(" Z=");
      Serial.println(shank.gyroZ, 2);
    } else {

      Serial.print("POST failed: ");
      Serial.println(http.errorToString(code));
    }

    http.end();

  } else {

    Serial.println("WiFi disconnected, skipping send.");
  }
}
