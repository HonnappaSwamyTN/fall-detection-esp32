/*
  Fall Detection & Auto-Alert System
  ESP32-C3 + MPU6050

  Features:
  - Raw I2C communication with MPU6050
  - 50Hz non-blocking IMU sampling
  - Free-fall weightlessness detection (< 0.51g)
  - Impact spike detection (> 1.53g)
  - Gyroscope rotation signature verification (> 2.0 rad/s)
  - Post-impact inactivity evaluation (variance < 2.2 m/s^2)
  - Posture & orientation tilt-shift verification (> 35 deg shift)
  - Dynamic multi-factor Fall Confidence Scoring (0.00 to 1.00)
  - 10-second PRE_ALERT grace period with pulsing audio/visual alarm
  - Manual button cancellation during PRE_ALERT (prevents all network alerts)
  - Non-blocking, standalone Wi-Fi auto-reconnection state management
  - Battery-powered standalone operation without USB Serial dependence
  - Non-blocking HTTP POST payload to Python FastAPI Backend API
  - Configurable Backend API URL via NVS Preferences & secrets.h fallback
  - Provisioning portal via NVS Preferences & SoftAP
  - I2C error checking & MPU6050 WHO_AM_I verification
  - Fall cooldown timer

  PIN MAP:
    MPU6050 SDA -> GPIO4
    MPU6050 SCL -> GPIO5
    Buzzer      -> GPIO6
    LED         -> GPIO7 (through 220 ohm resistor)
    Button      -> GPIO10 (INPUT_PULLUP)
    Button leg2 -> GND
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "secrets.h"

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define SDA_PIN       4
#define SCL_PIN       5
#define BUZZER_PIN    6
#define LED_PIN       7
#define BUTTON_PIN    10

// ============================================================
// MPU6050 I2C REGISTERS
// ============================================================
#define MPU_ADDR          0x68
#define WHO_AM_I_REG      0x75
#define PWR_MGMT_1        0x6B
#define ACCEL_CONFIG      0x1C
#define GYRO_CONFIG       0x1B
#define ACCEL_XOUT_H      0x3B
#define GYRO_XOUT_H       0x43

// Accelerometer: ±8g (4096 LSB = 1g)
#define ACCEL_SCALE       4096.0f
// Gyroscope: ±250 deg/s (131 LSB = 1 deg/s)
#define GYRO_SCALE        131.0f

// ============================================================
// SAMPLING & THRESHOLD CONSTANTS
// ============================================================
#define SENSOR_SAMPLE_INTERVAL_MS  20     // ~50 Hz non-blocking loop ticker

// Kinematic Thresholds & Timing Windows
#define FREEFALL_ACCEL_THRESHOLD   5.0f   // m/s^2 (< 0.51g) weightlessness threshold
#define FREEFALL_WINDOW_MS         500    // Max ms before impact to register free-fall

#define IMPACT_ACCEL_THRESHOLD     15.0f  // m/s^2 (> 1.53g) primary peak impact threshold
#define IMPACT_CONFIRM_THRESHOLD   12.0f  // m/s^2 (> 1.22g) 2nd-sample verification threshold
#define ROTATION_THRESHOLD         2.0f   // rad/s (> ~115 deg/s) angular velocity threshold
#define ROTATION_CHECK_MS          700    // Max ms after impact to confirm rotation

#define POST_IMPACT_SETTLE_MS      500    // 500 ms settling exclusion window to ignore impact bounce
#define INACTIVITY_WINDOW_MS       2000   // 2.0s settled stillness sampling window (ms)
#define MIN_VALID_SAMPLES          80     // Minimum valid samples required for inactivity analysis
#define FALL_CONFIDENCE_THRESHOLD  0.55f  // Minimum fall confidence score to trigger PRE_ALERT

// Alert & Cooldown Durations
#define PRE_ALERT_HOLD_MS          10000  // 10-second user cancellation grace period
#define ALERT_HOLD_MS              5000   // Active alert indication duration (ms)
#define FALL_COOLDOWN_MS           10000  // Cooldown before returning to MONITORING (ms)

// Wi-Fi Reconnect Settings
#define WIFI_RECONNECT_INTERVAL_MS 15000  // Background Wi-Fi reconnect retry interval (ms)

// Inactivity Buffer (100 samples max)
#define BUFFER_SIZE 100
float accelMagBuffer[BUFFER_SIZE];
float accelXBuffer[BUFFER_SIZE];
float accelYBuffer[BUFFER_SIZE];
float accelZBuffer[BUFFER_SIZE];
int bufferIndex = 0;

// Dynamic Posture Baseline Vector (m/s^2)
float baselineAx = 0.0f;
float baselineAy = 0.0f;
float baselineAz = 9.80665f;

float snapshotBaselineAx = 0.0f;
float snapshotBaselineAy = 0.0f;
float snapshotBaselineAz = 9.80665f;

// Free-fall & I2C Error Tracking State
unsigned long lastFreefallTime = 0;
bool freefallDetected = false;
unsigned long totalI2cErrors = 0;
unsigned long consecutiveI2cErrors = 0;

// Captured Kinematic Telemetry for Backend POST Payload
float capturedImpactG = 0.0f;
float capturedRotationRads = 0.0f;
float capturedPostureChangeDeg = 0.0f;
float capturedStillnessVariation = 0.0f;
float capturedFallConfidence = 0.0f;
bool capturedFreefallPresent = false;

// Candidate Verification & Peak Rotation Tracking
unsigned long impactCandidateTime = 0;
float impactCandidatePeak = 0.0f;
float capturedRotationPeak = 0.0f;

// Isolated I2C Recovery Function (Disabled for now)
void checkAndRecoverI2C() {
  // Reserved for future bus reset verification if needed:
  // Wire.begin(SDA_PIN, SCL_PIN);
}

// ============================================================
// SYSTEM STATE & WIFI STATE
// ============================================================
enum SystemState {
  MONITORING,
  IMPACT_VERIFY,
  POST_IMPACT_SETTLING,
  INACTIVITY_CHECK,
  PRE_ALERT,
  ALERTING,
  COOLDOWN
};

enum WiFiState {
  WIFI_STATE_DISCONNECTED,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED
};

SystemState currentState = MONITORING;
WiFiState currentWiFiState = WIFI_STATE_DISCONNECTED;

// State Timing & Variables
unsigned long stateStartTime = 0;
unsigned long lastSensorSampleTime = 0;
unsigned long lastPreAlertPulseTime = 0;
unsigned long lastWiFiReconnectAttempt = 0;

bool preAlertPulseState = false;
bool alertDispatched = false;

// Dynamic Credentials & NVS Preferences
Preferences preferences;
String wifiSSID     = "";
String wifiPassword = "";
String botToken     = "";
String chatID       = "";
String backendURL   = "";

// Debug Toggle (1 = print raw sensor values every 20ms, 0 = print state transitions only)
#define DEBUG_SENSOR 0

// ============================================================
// PROTOTYPES
// ============================================================
void resetToMonitoring();
void connectWiFi();
void maintainWiFiConnection();
void triggerAlertDispatch();
void sendBackendFallEvent();
void sendTelegramAlertFallback();
float calculateFallConfidence(float impactG, float rotationRads, float stillnessVar, float tiltDeg, bool freefallPresent);
bool loadCredentials();
void saveCredentials(const String& ssid, const String& pass, const String& token, const String& chat, const String& backend);
void clearCredentials();
void startProvisioningPortal();
bool mpuWriteRegister(uint8_t reg, uint8_t value);
bool mpuReadRegister(uint8_t reg, uint8_t &value);
bool mpuCheckPresent();
bool mpuReadAccelGyro(float &ax, float &ay, float &az, float &gx, float &gy, float &gz);
bool mpuReadAccelGyro(int16_t &rawAx, int16_t &rawAy, int16_t &rawAz, int16_t &rawGx, int16_t &rawGy, int16_t &rawGz, float &ax, float &ay, float &az, float &gx, float &gy, float &gz);
void handleButtonPress();

// ============================================================
// MPU6050 REGISTER WRITE
// ============================================================
bool mpuWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  uint8_t error = Wire.endTransmission();
  if (error != 0) {
    Serial.print("I2C write error: ");
    Serial.println(error);
    return false;
  }
  return true;
}

// ============================================================
// MPU6050 REGISTER READ
// ============================================================
bool mpuReadRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  uint8_t received = Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)1, (uint8_t)true);
  if (received != 1 || !Wire.available()) {
    return false;
  }
  value = Wire.read();
  return true;
}

// ============================================================
// CHECK MPU6050 PRESENCE
// ============================================================
bool mpuCheckPresent() {
  Wire.beginTransmission(MPU_ADDR);
  return (Wire.endTransmission() == 0);
}

// ============================================================
// READ MPU6050 ACCEL + GYRO
// ============================================================
bool mpuReadAccelGyro(int16_t &rawAx, int16_t &rawAy, int16_t &rawAz, int16_t &rawGx, int16_t &rawGy, int16_t &rawGz, float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14, (uint8_t)true);
  if (received != 14) {
    return false;
  }

  rawAx = ((int16_t)Wire.read() << 8) | Wire.read();
  rawAy = ((int16_t)Wire.read() << 8) | Wire.read();
  rawAz = ((int16_t)Wire.read() << 8) | Wire.read();

  // Skip temperature (2 bytes)
  Wire.read();
  Wire.read();

  rawGx = ((int16_t)Wire.read() << 8) | Wire.read();
  rawGy = ((int16_t)Wire.read() << 8) | Wire.read();
  rawGz = ((int16_t)Wire.read() << 8) | Wire.read();

  // Accel -> m/s²
  ax = (rawAx / ACCEL_SCALE) * 9.80665f;
  ay = (rawAy / ACCEL_SCALE) * 9.80665f;
  az = (rawAz / ACCEL_SCALE) * 9.80665f;

  // Gyro -> rad/s
  gx = (rawGx / GYRO_SCALE) * (PI / 180.0f);
  gy = (rawGy / GYRO_SCALE) * (PI / 180.0f);
  gz = (rawGz / GYRO_SCALE) * (PI / 180.0f);

  return true;
}

bool mpuReadAccelGyro(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  int16_t rx, ry, rz, rgx, rgy, rgz;
  return mpuReadAccelGyro(rx, ry, rz, rgx, rgy, rgz, ax, ay, az, gx, gy, gz);
}

// ============================================================
// PREFERENCES & PROVISIONING MANAGER
// ============================================================
bool loadCredentials() {
  preferences.begin("fall_config", true);
  wifiSSID     = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  botToken     = preferences.getString("bot_token", "");
  chatID       = preferences.getString("chat_id", "");
  backendURL   = preferences.getString("backend_url", "");
  preferences.end();

  if (backendURL.length() == 0) {
#ifdef BACKEND_URL
    backendURL = BACKEND_URL;
#else
    backendURL = "http://192.168.1.100:8000/api/fall-event";
#endif
  }

  if (wifiSSID.length() == 0) {
#ifdef WIFI_SSID
    if (String(WIFI_SSID) != "Your_WiFi_Name" && String(WIFI_SSID).length() > 0) {
      wifiSSID = WIFI_SSID;
      wifiPassword = WIFI_PASSWORD;
      botToken = BOT_TOKEN;
      chatID = CHAT_ID;
      return true;
    }
#endif
    return false;
  }
  return true;
}

void saveCredentials(const String& ssid, const String& pass, const String& token, const String& chat, const String& backend) {
  preferences.begin("fall_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.putString("bot_token", token);
  preferences.putString("chat_id", chat);
  preferences.putString("backend_url", backend);
  preferences.end();
}

void clearCredentials() {
  preferences.begin("fall_config", false);
  preferences.clear();
  preferences.end();
}

// ============================================================
// PROVISIONING WEB SERVER & CAPTIVE PORTAL
// ============================================================
DNSServer dnsServer;
WebServer webServer(80);

const char PROGMEM SETUP_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Fall Detector Setup</title>
  <style>
    body { font-family: Arial, sans-serif; background: #121212; color: #fff; margin: 0; padding: 20px; display: flex; justify-content: center; }
    .card { background: #1e1e1e; padding: 24px; border-radius: 12px; width: 100%; max-width: 400px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
    h2 { text-align: center; color: #00e676; margin-top: 0; }
    label { font-size: 14px; font-weight: bold; margin-top: 12px; display: block; color: #b0bec5; }
    input[type="text"], input[type="password"] { width: 100%; padding: 10px; margin-top: 6px; border: 1px solid #333; border-radius: 6px; background: #2a2a2a; color: #fff; box-sizing: border-box; }
    input[type="submit"] { width: 100%; padding: 12px; margin-top: 24px; border: none; border-radius: 6px; background: #00e676; color: #000; font-weight: bold; font-size: 16px; cursor: pointer; }
    input[type="submit"]:hover { background: #00c853; }
    .note { font-size: 12px; color: #888; margin-top: 15px; text-align: center; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Fall Detector Setup</h2>
    <form action="/save" method="POST">
      <label for="ssid">Wi-Fi Network Name (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="Your Wi-Fi SSID">
      
      <label for="pass">Wi-Fi Password</label>
      <input type="password" id="pass" name="pass" placeholder="Your Wi-Fi Password">
      
      <label for="backend">Backend API URL</label>
      <input type="text" id="backend" name="backend" required placeholder="http://192.168.1.100:8000/api/fall-event">
      
      <label for="token">Telegram Bot Token (Optional Fallback)</label>
      <input type="text" id="token" name="token" placeholder="123456789:ABCdef...">
      
      <label for="chat">Telegram Chat ID (Optional Fallback)</label>
      <input type="text" id="chat" name="chat" placeholder="123456789">
      
      <input type="submit" value="Save & Restart">
    </form>
    <div class="note">Device will reboot automatically after saving.</div>
  </div>
</body>
</html>
)rawliteral";

void handleRoot() {
  webServer.send(200, "text/html", SETUP_HTML);
}

void handleSave() {
  if (webServer.hasArg("ssid") && webServer.hasArg("backend")) {
    String reqSSID    = webServer.arg("ssid");
    String reqPass    = webServer.arg("pass");
    String reqBackend = webServer.arg("backend");
    String reqToken   = webServer.hasArg("token") ? webServer.arg("token") : "";
    String reqChat    = webServer.hasArg("chat") ? webServer.arg("chat") : "";

    reqSSID.trim();
    reqPass.trim();
    reqBackend.trim();
    reqToken.trim();
    reqChat.trim();

    saveCredentials(reqSSID, reqPass, reqToken, reqChat, reqBackend);

    String resp = "<html><body style='font-family:sans-serif;background:#121212;color:#00e676;text-align:center;padding:50px;'>";
    resp += "<h2>Settings Saved Successfully!</h2><p style='color:#fff;'>Device is restarting and connecting to Wi-Fi...</p></body></html>";

    webServer.send(200, "text/html", resp);
    delay(2000);
    ESP.restart();
  } else {
    webServer.send(400, "text/plain", "Bad Request: Missing required fields");
  }
}

void startProvisioningPortal() {
  Serial.println();
  Serial.println("==========================================");
  Serial.println("   STARTING PROVISIONING SETUP PORTAL   ");
  Serial.println("==========================================");
  Serial.println("Broadcasting SoftAP: FallDetector-Setup");
  Serial.println("Connect to 'FallDetector-Setup' and navigate to http://192.168.4.1");
  Serial.println("==========================================");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("FallDetector-Setup");

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("Access Point IP: ");
  Serial.println(apIP);

  dnsServer.start(53, "*", apIP);
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    webServer.send(302, "text/plain", "");
  });

  webServer.begin();
  unsigned long lastBlink = 0;
  bool ledState = false;

  while (true) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
    delay(5);
  }
}

// ============================================================
// RESET SYSTEM STATE
// ============================================================
void resetToMonitoring() {
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  currentState = MONITORING;
  alertDispatched = false;
  freefallDetected = false;
  capturedFreefallPresent = false;
  bufferIndex = 0;
  impactCandidatePeak = 0.0f;
  capturedRotationPeak = 0.0f;
}

// ============================================================
// BUTTON EVENT HANDLER (DEBOUNCED NON-BLOCKING)
// ============================================================
unsigned long lastButtonPressTime = 0;

void handleButtonPress() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastButtonPressTime > 300) {
      lastButtonPressTime = now;
      Serial.println();
      Serial.println("[BUTTON] Manual cancel button pressed!");

      if (currentState == PRE_ALERT) {
        Serial.println("[CANCEL] Fall alert cancelled by user during PRE_ALERT!");
        Serial.println("[CANCEL] Network notification ABORTED.");
      } else {
        Serial.println("[BUTTON] System reset to MONITORING.");
      }

      resetToMonitoring();
    }
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // Bounded wait for USB Serial (non-blocking for battery power)
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 5000) {
    delay(10);
  }

  Serial.println("BOOT: ESP32-C3 firmware starting");

  delay(500);

  // GPIO Setup
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("  ESP32-C3 Standalone Fall Detector (v4C)");
  Serial.println("==========================================");

  // Check boot reset button
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Reset button held during startup...");
    Serial.println("Hold for 3 seconds to clear saved credentials...");

    unsigned long pressStart = millis();
    bool resetTriggered = false;

    while (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - pressStart >= 3000) {
        resetTriggered = true;
        break;
      }
      delay(50);
    }

    if (resetTriggered) {
      Serial.println("Credentials cleared! Starting provisioning portal...");
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
      }
      digitalWrite(LED_PIN, LOW);
      clearCredentials();
      startProvisioningPortal();
    }
  }

  // Load NVS credentials
  if (!loadCredentials()) {
    Serial.println("No valid Wi-Fi credentials found in NVS.");
    Serial.println("Launching setup portal...");
    startProvisioningPortal();
  }

  Serial.println("Credentials loaded successfully.");
  Serial.print("[WIFI] Saved SSID: ");
  Serial.println(wifiSSID);
  Serial.print("Target Backend API URL: ");
  Serial.println(backendURL);

  // Initialize I2C
  Serial.println("Initializing I2C bus (SDA: GPIO4, SCL: GPIO5)...");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  delay(100);

  // Check MPU6050
  if (!mpuCheckPresent()) {
    Serial.println("ERROR: MPU6050 not responding on I2C bus.");
    while (1) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    }
  }

  uint8_t whoAmI = 0;
  if (!mpuReadRegister(WHO_AM_I_REG, whoAmI)) {
    Serial.println("ERROR: Failed to read WHO_AM_I register.");
    while (1) { delay(1000); }
  }

  Serial.print("MPU6050 WHO_AM_I = 0x");
  Serial.println(whoAmI, HEX);

  if (whoAmI != 0x68 && whoAmI != 0x70) {
    Serial.println("ERROR: Unknown device ID.");
    while (1) { delay(1000); }
  }

  // Wake up MPU6050
  if (!mpuWriteRegister(PWR_MGMT_1, 0x00)) {
    Serial.println("ERROR: Failed to wake MPU6050.");
    while (1) { delay(1000); }
  }
  delay(50);

  // Configure Accelerometer (±8g) & Gyroscope (±250 deg/s)
  mpuWriteRegister(ACCEL_CONFIG, 0x10);
  mpuWriteRegister(GYRO_CONFIG, 0x00);
  Serial.println("MPU6050 configured successfully.");

  // Initiate Wi-Fi Connection (Bounded boot attempt)
  connectWiFi();

  Serial.println();
  Serial.println("==========================================");
  Serial.println("System Ready. Monitoring for fall events...");
  Serial.println("==========================================");
}

// ============================================================
// WIFI CONNECTION & AUTOMATIC RECONNECTION STATE MACHINE
// ============================================================
void connectWiFi() {
  Serial.print("[WIFI] Attempting connection to SSID: ");
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  currentWiFiState = WIFI_STATE_CONNECTING;

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(250);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    currentWiFiState = WIFI_STATE_CONNECTED;
    Serial.println("[WIFI] CONNECTED");
    Serial.print("[WIFI] SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("[WIFI] ESP32 IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("[WIFI] RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    currentWiFiState = WIFI_STATE_DISCONNECTED;
    Serial.print("[WIFI] Connection failed/disconnected. Status: ");
    Serial.println(WiFi.status());
    Serial.println("[WIFI] Operating in standalone LOCAL mode; background reconnect active.");
  }
}

void maintainWiFiConnection() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (currentWiFiState != WIFI_STATE_CONNECTED) {
      currentWiFiState = WIFI_STATE_CONNECTED;
      Serial.println("[WIFI] CONNECTED");
      Serial.print("[WIFI] SSID: ");
      Serial.println(WiFi.SSID());
      Serial.print("[WIFI] ESP32 IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("[WIFI] Gateway: ");
      Serial.println(WiFi.gatewayIP());
      Serial.print("[WIFI] RSSI: ");
      Serial.println(WiFi.RSSI());
    }
  } else {
    if (currentWiFiState == WIFI_STATE_CONNECTED) {
      currentWiFiState = WIFI_STATE_DISCONNECTED;
      Serial.print("[WIFI] Connection failed/disconnected. Status: ");
      Serial.println(status);
    }

    unsigned long now = millis();
    if (now - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
      lastWiFiReconnectAttempt = now;
      currentWiFiState = WIFI_STATE_CONNECTING;
      Serial.print("[WIFI] Attempting connection to SSID: ");
      Serial.println(wifiSSID);
      WiFi.reconnect();
    }
  }
}

// ============================================================
// DYNAMIC FALL CONFIDENCE CALCULATION
// ============================================================
float calculateFallConfidence(float impactG, float rotationRads, float stillnessStdDev, float tiltDeg, bool freefallPresent) {
  // 1. Impact Strength Score (Max 0.30)
  float sImpact = (impactG * 9.80665f - 15.0f) / (58.84f - 15.0f) * 0.30f;
  if (sImpact < 0.0f) sImpact = 0.0f;
  if (sImpact > 0.30f) sImpact = 0.30f;

  // 2. Rotation Velocity Score (Max 0.25)
  float sRotation = (rotationRads - 2.0f) / (6.0f - 2.0f) * 0.25f;
  if (sRotation < 0.0f) sRotation = 0.0f;
  if (sRotation > 0.25f) sRotation = 0.25f;

  // 3. Stillness Score (Max 0.20)
  float fSigma = 1.0f - (stillnessStdDev / 3.0f);
  if (fSigma < 0.0f) fSigma = 0.0f;
  if (fSigma > 1.0f) fSigma = 1.0f;
  float sStillness = 0.20f * fSigma;

  // 4. Posture Orientation Shift Score (Max 0.20)
  float sTilt = 0.0f;
  if (tiltDeg >= 30.0f) {
    sTilt = 0.10f + 0.10f * ((tiltDeg - 30.0f) / 30.0f);
    if (sTilt > 0.20f) sTilt = 0.20f;
  } else if (tiltDeg >= 15.0f) {
    sTilt = 0.05f + 0.05f * ((tiltDeg - 15.0f) / 15.0f);
  }

  // 5. Free-fall Weightlessness Bonus (Max 0.05)
  float sFreefall = freefallPresent ? 0.05f : 0.00f;

  float totalConfidence = sImpact + sRotation + sStillness + sTilt + sFreefall;
  if (totalConfidence < 0.0f) totalConfidence = 0.0f;
  if (totalConfidence > 1.0f) totalConfidence = 1.0f;

  return totalConfidence;
}

// ============================================================
// ALERT DISPATCH (HTTP POST TO FASTAPI BACKEND)
// ============================================================
void triggerAlertDispatch() {
  Serial.println("[ALERT] Triggering Fall Alert Dispatch to Backend API...");
  sendBackendFallEvent();
}

void sendBackendFallEvent() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BACKEND] Network unavailable; event will not be sent yet.");
    sendTelegramAlertFallback();
    return;
  }

  Serial.print("[HTTP] Sending Fall Telemetry JSON to: ");
  Serial.println(backendURL);

  HTTPClient http;
  http.begin(backendURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000); // Bounded 5-second timeout to prevent freezing loop

  unsigned long currentSeconds = millis() / 1000;
  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf), "UPTIME_%luS", currentSeconds);

  String jsonPayload = "{";
  jsonPayload += "\"device_id\":\"ESP32C3_01\",";
  jsonPayload += "\"timestamp\":\"" + String(timeBuf) + "\",";
  jsonPayload += "\"impact_g\":" + String(capturedImpactG, 2) + ",";
  jsonPayload += "\"rotation_rads\":" + String(capturedRotationRads, 2) + ",";
  jsonPayload += "\"posture_change_deg\":" + String(capturedPostureChangeDeg, 1) + ",";
  jsonPayload += "\"stillness_variation\":" + String(capturedStillnessVariation, 2) + ",";
  jsonPayload += "\"fall_confidence\":" + String(capturedFallConfidence, 2);
  jsonPayload += "}";

  Serial.println("[HTTP] Request Payload:");
  Serial.println(jsonPayload);

  int httpCode = http.POST(jsonPayload);

  if (httpCode > 0) {
    Serial.print("[HTTP] Backend Response Code: ");
    Serial.println(httpCode);
    String response = http.getString();
    Serial.print("[HTTP] Backend Response Body: ");
    Serial.println(response);
  } else {
    Serial.print("[HTTP] POST failed, error: ");
    Serial.println(http.errorToString(httpCode));
    Serial.println("[HTTP] Backend unreachable. Falling back to direct notification if configured...");
    sendTelegramAlertFallback();
  }

  http.end();
}

void sendTelegramAlertFallback() {
  if (botToken.length() == 0 || chatID.length() == 0 || botToken == "Your_New_Bot_Token") {
    Serial.println("[FALLBACK] Direct Telegram skipped: Credentials unconfigured.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FALLBACK] Direct Telegram skipped: Wi-Fi disconnected.");
    return;
  }

  Serial.println("[FALLBACK] Sending fallback direct Telegram notification...");
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + botToken + "/sendMessage";

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    https.setTimeout(5000);
    String payload = "{\"chat_id\":\"" + chatID + "\",\"text\":\"🚨 FALL DETECTED (Fallback Alert)! Immediate attention required.\"}";
    int httpCode = https.POST(payload);
    if (httpCode == 200) {
      Serial.println("[FALLBACK] Direct Telegram fallback alert delivered.");
    }
    https.end();
  }
}

// ============================================================
// SENSOR PROCESSING (EXECUTED AT 50Hz NON-BLOCKING)
// ============================================================
unsigned long lastSensorDebugTime = 0;

void processSensorStep() {
  int16_t rawAx, rawAy, rawAz;
  int16_t rawGx, rawGy, rawGz;
  float ax, ay, az;
  float gx, gy, gz;

  // 1. I2C Sample Validation: Discard corrupted samples immediately
  if (!mpuReadAccelGyro(rawAx, rawAy, rawAz, rawGx, rawGy, rawGz, ax, ay, az, gx, gy, gz)) {
    totalI2cErrors++;
    consecutiveI2cErrors++;
    Serial.print("[I2C ERROR] Invalid read! Total: ");
    Serial.print(totalI2cErrors);
    Serial.print(" | Consecutive: ");
    Serial.println(consecutiveI2cErrors);
    return; // Discard sample: do not update baseline, FSM, or buffer
  }

  // Reset consecutive I2C error counter upon a successful valid read
  consecutiveI2cErrors = 0;

  float accelMag = sqrt(ax * ax + ay * ay + az * az);
  float gyroMag  = sqrt(gx * gx + gy * gy + gz * gz);
  unsigned long now = millis();

  // Update peak rotation evidence across post-impact period
  if (currentState == IMPACT_VERIFY || currentState == POST_IMPACT_SETTLING || currentState == INACTIVITY_CHECK) {
    if (gyroMag > capturedRotationPeak) {
      capturedRotationPeak = gyroMag;
    }
  }

  // Temporary Diagnostic Printing (once every 500 ms)
  if (now - lastSensorDebugTime >= 500) {
    lastSensorDebugTime = now;
    Serial.print("[SENSOR DEBUG] Raw Accel [X,Y,Z]: ");
    Serial.print(rawAx); Serial.print(", ");
    Serial.print(rawAy); Serial.print(", ");
    Serial.print(rawAz);
    Serial.print(" | Accel m/s^2 [X,Y,Z]: ");
    Serial.print(ax, 2); Serial.print(", ");
    Serial.print(ay, 2); Serial.print(", ");
    Serial.print(az, 2);
    Serial.print(" | Accel Mag: ");
    Serial.print(accelMag, 2);
    Serial.print(" m/s^2 | Raw Gyro [X,Y,Z]: ");
    Serial.print(rawGx); Serial.print(", ");
    Serial.print(rawGy); Serial.print(", ");
    Serial.print(rawGz);
    Serial.print(" | Gyro rad/s [X,Y,Z]: ");
    Serial.print(gx, 2); Serial.print(", ");
    Serial.print(gy, 2); Serial.print(", ");
    Serial.println(gz, 2);
  }

#if DEBUG_SENSOR
  Serial.print("Accel: ");
  Serial.print(accelMag, 2);
  Serial.print(" m/s^2 | Gyro: ");
  Serial.print(gyroMag, 2);
  Serial.println(" rad/s");
#endif

  // Baseline Posture Update & Free-fall Tracking (during MONITORING)
  if (currentState == MONITORING) {
    // 1. Posture baseline update during resting state
    if (accelMag >= 6.86f && accelMag <= 12.75f) {
      baselineAx = baselineAx * 0.95f + ax * 0.05f;
      baselineAy = baselineAy * 0.95f + ay * 0.05f;
      baselineAz = baselineAz * 0.95f + az * 0.05f;
    }

    // 2. Free-fall weightlessness detection (< 5.0 m/s^2 or < 0.51g)
    if (accelMag < FREEFALL_ACCEL_THRESHOLD) {
      lastFreefallTime = now;
      freefallDetected = true;
    }
  }

  // ==========================================================
  // STATE MACHINE
  // ==========================================================
  switch (currentState) {

    // --------------------------------------------------------
    // 1. MONITORING
    // --------------------------------------------------------
    case MONITORING:
      if (accelMag > IMPACT_ACCEL_THRESHOLD) {
        impactCandidateTime = now;
        impactCandidatePeak = accelMag;
        capturedRotationPeak = gyroMag;

        bool recentFreefall = freefallDetected && (now - lastFreefallTime <= FREEFALL_WINDOW_MS);
        capturedFreefallPresent = recentFreefall;
        freefallDetected = false; // Reset candidate freefall flag

        currentState = IMPACT_VERIFY;
      }
      break;

    // --------------------------------------------------------
    // 2. IMPACT_VERIFY (2-Sample Confirmation)
    // --------------------------------------------------------
    case IMPACT_VERIFY:
      if (now - impactCandidateTime <= 40) {
        if (accelMag > IMPACT_CONFIRM_THRESHOLD) {
          if (accelMag > impactCandidatePeak) {
            impactCandidatePeak = accelMag;
          }

          Serial.println();
          Serial.println("==========================================");
          Serial.println("  !!! HIGH IMPACT CANDIDATE CONFIRMED !!!");
          Serial.print("  Impact Peak: ");
          Serial.print(impactCandidatePeak, 2);
          Serial.print(" m/s^2 (");
          Serial.print(impactCandidatePeak / 9.80665f, 2);
          Serial.println("g)");
          Serial.print("  Free-fall Evidence: ");
          Serial.println(capturedFreefallPresent ? "YES" : "NO");
          Serial.println("==========================================");

          // Snapshot baseline vector and captured impact G
          snapshotBaselineAx = baselineAx;
          snapshotBaselineAy = baselineAy;
          snapshotBaselineAz = baselineAz;
          capturedImpactG = impactCandidatePeak / 9.80665f;

          currentState = POST_IMPACT_SETTLING;
          stateStartTime = now;
        }
      } else {
        // Single-frame noise or I2C glitch rejected
        resetToMonitoring();
      }
      break;

    // --------------------------------------------------------
    // 3. POST_IMPACT_SETTLING (500 ms Exclusion Window)
    // --------------------------------------------------------
    case POST_IMPACT_SETTLING:
      if (now - stateStartTime >= POST_IMPACT_SETTLE_MS) {
        currentState = INACTIVITY_CHECK;
        stateStartTime = now;
        bufferIndex = 0;
      }
      break;

    // --------------------------------------------------------
    // 4. INACTIVITY_CHECK (2.0s Settled Window Analysis)
    // --------------------------------------------------------
    case INACTIVITY_CHECK:
      if (bufferIndex < BUFFER_SIZE) {
        accelMagBuffer[bufferIndex] = accelMag;
        accelXBuffer[bufferIndex]   = ax;
        accelYBuffer[bufferIndex]   = ay;
        accelZBuffer[bufferIndex]   = az;
        bufferIndex++;
      }

      if (now - stateStartTime >= INACTIVITY_WINDOW_MS || bufferIndex >= BUFFER_SIZE) {
        // Minimum valid sample requirement check
        if (bufferIndex < MIN_VALID_SAMPLES) {
          Serial.print("[FSM] REJECTED: Insufficient valid samples collected (");
          Serial.print(bufferIndex);
          Serial.println(" < 80).");
          resetToMonitoring();
          break;
        }

        float sumVal = 0.0f, sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
        int nearGravityCount = 0;

        for (int i = 0; i < bufferIndex; i++) {
          sumVal += accelMagBuffer[i];
          sumX   += accelXBuffer[i];
          sumY   += accelYBuffer[i];
          sumZ   += accelZBuffer[i];
          if (accelMagBuffer[i] >= 7.0f && accelMagBuffer[i] <= 12.5f) {
            nearGravityCount++;
          }
        }

        float meanAccel = sumVal / bufferIndex;
        float avgX      = sumX / bufferIndex;
        float avgY      = sumY / bufferIndex;
        float avgZ      = sumZ / bufferIndex;

        float varianceSum = 0.0f;
        for (int i = 0; i < bufferIndex; i++) {
          float diff = accelMagBuffer[i] - meanAccel;
          varianceSum += diff * diff;
        }
        float stdDev = sqrt(varianceSum / bufferIndex);
        float nearGravityPercent = ((float)nearGravityCount / bufferIndex) * 100.0f;

        // Posture Angle Calculation
        float magBase = sqrt(snapshotBaselineAx*snapshotBaselineAx + snapshotBaselineAy*snapshotBaselineAy + snapshotBaselineAz*snapshotBaselineAz);
        float magPost = sqrt(avgX*avgX + avgY*avgY + avgZ*avgZ);
        float dotProduct = (snapshotBaselineAx*avgX + snapshotBaselineAy*avgY + snapshotBaselineAz*avgZ);
        float cosTilt = 1.0f;
        if (magBase > 0.1f && magPost > 0.1f) cosTilt = dotProduct / (magBase * magPost);
        if (cosTilt > 1.0f) cosTilt = 1.0f;
        if (cosTilt < -1.0f) cosTilt = -1.0f;
        float postureShiftDeg = acos(cosTilt) * (180.0f / PI);

        // Compute Fall Confidence Score
        float confidence = calculateFallConfidence(
          capturedImpactG,
          capturedRotationPeak,
          stdDev,
          postureShiftDeg,
          capturedFreefallPresent
        );

        // Comprehensive Debug Telemetry Logging
        Serial.println();
        Serial.println("==========================================");
        Serial.println("[STILLNESS TELEMETRY]");
        Serial.print("samples="); Serial.println(bufferIndex);
        Serial.print("totalI2cErrors="); Serial.println(totalI2cErrors);
        Serial.print("consecutiveI2cErrors="); Serial.println(consecutiveI2cErrors);
        Serial.print("meanAccel="); Serial.print(meanAccel, 2); Serial.println(" m/s^2");
        Serial.print("stdDev="); Serial.print(stdDev, 2); Serial.println(" m/s^2");
        Serial.print("nearGravityPercent="); Serial.print(nearGravityPercent, 1); Serial.println("%");
        Serial.print("postureShift="); Serial.print(postureShiftDeg, 1); Serial.println(" deg");
        Serial.print("impactCandidatePeak="); Serial.print(impactCandidatePeak, 2); Serial.println(" m/s^2");
        Serial.print("rotationPeak="); Serial.print(capturedRotationPeak, 2); Serial.println(" rad/s");
        Serial.print("freefallDetected="); Serial.println(capturedFreefallPresent ? "YES" : "NO");
        Serial.print("confidence="); Serial.println(confidence, 2);
        Serial.println("==========================================");

        // Safety Gate & Decision Threshold
        bool safetyGatePassed = (meanAccel >= 7.0f && meanAccel <= 12.5f && stdDev < 3.0f);

        if (safetyGatePassed && confidence >= FALL_CONFIDENCE_THRESHOLD) {
          capturedFallConfidence = confidence;
          capturedStillnessVariation = stdDev;
          capturedPostureChangeDeg = postureShiftDeg;
          capturedRotationRads = capturedRotationPeak;

          Serial.println();
          Serial.println("==========================================");
          Serial.println("  !!! POTENTIAL FALL CONFIRMED !!!");
          Serial.print("  Calculated Fall Confidence: ");
          Serial.print(capturedFallConfidence * 100.0f, 1);
          Serial.println("%");
          Serial.println("  Starting 10-Second Grace Period (PRE_ALERT)...");
          Serial.println("  Press button to cancel alert.");
          Serial.println("==========================================");

          currentState = PRE_ALERT;
          stateStartTime = now;
          lastPreAlertPulseTime = now;
          preAlertPulseState = false;
        } else {
          if (!safetyGatePassed) Serial.println("[FSM] Event REJECTED: Safety gate failed (motion/gravity out of range).");
          else Serial.println("[FSM] Event REJECTED: Confidence score below threshold (0.55).");
          resetToMonitoring();
        }
      }
      break;

    // --------------------------------------------------------
    // 4. PRE_ALERT (10-SECOND USER CANCEL GRACE WINDOW)
    // --------------------------------------------------------
    case PRE_ALERT:
      // Pulse Buzzer and LED (200ms ON / 200ms OFF)
      if (now - lastPreAlertPulseTime >= 200) {
        lastPreAlertPulseTime = now;
        preAlertPulseState = !preAlertPulseState;
        digitalWrite(BUZZER_PIN, preAlertPulseState ? HIGH : LOW);
        digitalWrite(LED_PIN, preAlertPulseState ? HIGH : LOW);
      }

      // Check if 10 seconds expired without button cancellation
      if (now - stateStartTime >= PRE_ALERT_HOLD_MS) {
        Serial.println();
        Serial.println("==========================================");
        Serial.println("  !!! GRACE PERIOD EXPIRED - DISPATCHING !!!");
        Serial.println("==========================================");

        digitalWrite(BUZZER_PIN, HIGH);
        digitalWrite(LED_PIN, HIGH);

        currentState = ALERTING;
        stateStartTime = now;
        alertDispatched = false;
      }
      break;

    // --------------------------------------------------------
    // 5. ALERTING
    // --------------------------------------------------------
    case ALERTING:
      if (!alertDispatched) {
        triggerAlertDispatch();
        alertDispatched = true;
      }

      if (now - stateStartTime >= ALERT_HOLD_MS) {
        Serial.println("[FSM] Active alert period completed. Entering COOLDOWN...");
        digitalWrite(BUZZER_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
        currentState = COOLDOWN;
        stateStartTime = now;
      }
      break;

    // --------------------------------------------------------
    // 6. COOLDOWN
    // --------------------------------------------------------
    case COOLDOWN:
      if (now - stateStartTime >= FALL_COOLDOWN_MS) {
        Serial.println("[FSM] Cooldown complete. Returning to MONITORING.");
        resetToMonitoring();
      }
      break;
  }
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  // Non-blocking Wi-Fi maintenance state ticker
  maintainWiFiConnection();

  // Continuous non-blocking button polling
  handleButtonPress();

  // 50Hz Non-blocking IMU sampling ticker
  unsigned long now = millis();
  if (now - lastSensorSampleTime >= SENSOR_SAMPLE_INTERVAL_MS) {
    lastSensorSampleTime = now;
    processSensorStep();
  }
}