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
  - Non-blocking HTTP POST payload to Python FastAPI Backend API
  - Configurable Backend API URL via NVS Preferences & secrets.h fallback
  - Provisioning portal via NVS Preferences
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

#define IMPACT_ACCEL_THRESHOLD     15.0f  // m/s^2 (> 1.53g) peak impact threshold
#define ROTATION_THRESHOLD         2.0f   // rad/s (> ~115 deg/s) angular velocity threshold
#define ROTATION_CHECK_MS          700    // Max ms after impact to confirm rotation

#define INACTIVITY_WINDOW_MS       2500   // Post-impact stillness sampling window (ms)
#define INACTIVITY_VARIATION_MAX   2.2f   // Max post-impact acceleration variance for stillness
#define POSTURE_TILT_COS_MAX       0.819f // cos(35 deg): orientation tilt change > 35 degrees

// Alert & Cooldown Durations
#define PRE_ALERT_HOLD_MS          10000  // 10-second user cancellation grace period
#define ALERT_HOLD_MS              5000   // Active alert indication duration (ms)
#define FALL_COOLDOWN_MS           10000  // Cooldown before returning to MONITORING (ms)

// Inactivity Buffer
#define BUFFER_SIZE 50
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

// Free-fall Tracking State
unsigned long lastFreefallTime = 0;
bool freefallDetected = false;

// Captured Kinematic Telemetry for Backend POST Payload
float capturedImpactG = 0.0f;
float capturedRotationRads = 0.0f;
float capturedPostureChangeDeg = 0.0f;
float capturedStillnessVariation = 0.0f;
float capturedFallConfidence = 0.0f;
bool capturedFreefallPresent = false;

// ============================================================
// SYSTEM STATE
// ============================================================
enum SystemState {
  MONITORING,
  ROTATION_CHECK,
  INACTIVITY_CHECK,
  PRE_ALERT,
  ALERTING,
  COOLDOWN
};

SystemState currentState = MONITORING;

// State Timing & Variables
unsigned long stateStartTime = 0;
unsigned long lastSensorSampleTime = 0;
unsigned long lastPreAlertPulseTime = 0;
bool preAlertPulseState = false;
bool rotationConfirmed = false;
bool alertDispatched = false;

// Dynamic Credentials & NVS Preferences
Preferences preferences;
String wifiSSID     = "";
String wifiPassword = "";
String botToken     = "";
String chatID       = "";
String backendURL   = "";

// Debug Toggle (1 = print sensor values every 20ms, 0 = print state transitions only)
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
bool mpuReadAccelGyro(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14, (uint8_t)true);
  if (received != 14) {
    return false;
  }

  int16_t rawAx = ((int16_t)Wire.read() << 8) | Wire.read();
  int16_t rawAy = ((int16_t)Wire.read() << 8) | Wire.read();
  int16_t rawAz = ((int16_t)Wire.read() << 8) | Wire.read();

  // Skip temperature (2 bytes)
  Wire.read();
  Wire.read();

  int16_t rawGx = ((int16_t)Wire.read() << 8) | Wire.read();
  int16_t rawGy = ((int16_t)Wire.read() << 8) | Wire.read();
  int16_t rawGz = ((int16_t)Wire.read() << 8) | Wire.read();

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
  rotationConfirmed = false;
  alertDispatched = false;
  freefallDetected = false;
  bufferIndex = 0;
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
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }

  delay(1000);

  // GPIO Setup
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("  ESP32-C3 Fall Detection System (v4.0)  ");
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

  // Wi-Fi Connection
  connectWiFi();

  Serial.println();
  Serial.println("==========================================");
  Serial.println("System Ready. Monitoring for fall events...");
  Serial.println("==========================================");
}

// ============================================================
// WIFI CONNECTION & MAINTENANCE
// ============================================================
void connectWiFi() {
  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi Connected! IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed/timed out.");
    Serial.println("System will operate in LOCAL ALARM mode.");
  }
}

unsigned long lastWiFiReconnectAttempt = 0;

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiReconnectAttempt >= 30000) {
      lastWiFiReconnectAttempt = now;
      Serial.println("[WIFI] Disconnected. Triggering background reconnect...");
      WiFi.reconnect();
    }
  }
}

// ============================================================
// DYNAMIC FALL CONFIDENCE CALCULATION
// ============================================================
float calculateFallConfidence(float impactG, float rotationRads, float stillnessVar, float tiltDeg, bool freefallPresent) {
  // 1. Impact Strength Score (Max 0.30)
  // Baseline threshold = 1.53g (15.0 m/s^2). Full 0.30 score at >= 6.0g
  float sImpact = (impactG / 6.0f) * 0.30f;
  if (sImpact > 0.30f) sImpact = 0.30f;

  // 2. Rotation Velocity Score (Max 0.25)
  // Baseline threshold = 2.0 rad/s (~115 deg/s). Full 0.25 score at >= 6.0 rad/s
  float sRotation = (rotationRads / 6.0f) * 0.25f;
  if (sRotation > 0.25f) sRotation = 0.25f;

  // 3. Stillness & Inactivity Score (Max 0.20)
  // Max variation threshold = 2.2 m/s^2. Lower variation -> higher score
  float sStillness = 0.20f * (1.0f - (stillnessVar / INACTIVITY_VARIATION_MAX));
  if (sStillness < 0.05f) sStillness = 0.05f;
  if (sStillness > 0.20f) sStillness = 0.20f;

  // 4. Posture Orientation Shift Score (Max 0.20)
  // Full 0.20 score at >= 90 deg tilt shift (going from vertical upright to horizontal flat)
  float sTilt = (tiltDeg / 90.0f) * 0.20f;
  if (sTilt > 0.20f) sTilt = 0.20f;

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
    Serial.println("[HTTP] Alert POST skipped: Wi-Fi not connected.");
    sendTelegramAlertFallback();
    return;
  }

  Serial.print("[HTTP] Sending Fall Telemetry JSON to: ");
  Serial.println(backendURL);

  HTTPClient http;
  http.begin(backendURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000); // 5-second timeout to prevent freezing loop

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
void processSensorStep() {
  float ax, ay, az;
  float gx, gy, gz;

  if (!mpuReadAccelGyro(ax, ay, az, gx, gy, gz)) {
    return;
  }

  float accelMag = sqrt(ax * ax + ay * ay + az * az);
  float gyroMag  = sqrt(gx * gx + gy * gy + gz * gz);

#if DEBUG_SENSOR
  Serial.print("Accel: ");
  Serial.print(accelMag, 2);
  Serial.print(" m/s^2 | Gyro: ");
  Serial.print(gyroMag, 2);
  Serial.println(" rad/s");
#endif

  unsigned long now = millis();

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
        // Check if free-fall occurred within the recent timing window before impact
        bool recentFreefall = freefallDetected && (now - lastFreefallTime <= FREEFALL_WINDOW_MS);

        Serial.println();
        Serial.println("==========================================");
        Serial.println("  !!! HIGH IMPACT DETECTED !!!");
        Serial.print("  Impact Peak: ");
        Serial.print(accelMag, 2);
        Serial.print(" m/s^2 (");
        Serial.print(accelMag / 9.80665f, 2);
        Serial.println("g)");
        Serial.print("  Free-fall Evidence: ");
        Serial.println(recentFreefall ? "YES" : "NO");
        Serial.println("==========================================");

        // Snapshot kinematic measurements & baseline
        snapshotBaselineAx = baselineAx;
        snapshotBaselineAy = baselineAy;
        snapshotBaselineAz = baselineAz;

        capturedImpactG = accelMag / 9.80665f;
        capturedRotationRads = gyroMag;
        capturedFreefallPresent = recentFreefall;

        currentState = ROTATION_CHECK;
        stateStartTime = now;
        rotationConfirmed = (gyroMag > ROTATION_THRESHOLD);
      }
      break;

    // --------------------------------------------------------
    // 2. ROTATION CHECK
    // --------------------------------------------------------
    case ROTATION_CHECK:
      if (gyroMag > ROTATION_THRESHOLD) {
        rotationConfirmed = true;
      }
      if (gyroMag > capturedRotationRads) {
        capturedRotationRads = gyroMag;
      }

      if (now - stateStartTime >= ROTATION_CHECK_MS) {
        if (rotationConfirmed) {
          Serial.println("[FSM] Gyro rotation signature CONFIRMED.");
          Serial.println("[FSM] Checking post-impact stillness & posture shift...");
          currentState = INACTIVITY_CHECK;
          stateStartTime = now;
          bufferIndex = 0;
        } else {
          Serial.println("[FSM] No rotation detected. Impact event REJECTED.");
          resetToMonitoring();
        }
      }
      break;

    // --------------------------------------------------------
    // 3. INACTIVITY CHECK
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
        if (bufferIndex == 0) {
          resetToMonitoring();
          break;
        }

        // Calculate min, max, average magnitude
        float minVal = accelMagBuffer[0];
        float maxVal = accelMagBuffer[0];
        float sumVal = accelMagBuffer[0];
        float sumX   = accelXBuffer[0];
        float sumY   = accelYBuffer[0];
        float sumZ   = accelZBuffer[0];

        for (int i = 1; i < bufferIndex; i++) {
          if (accelMagBuffer[i] < minVal) minVal = accelMagBuffer[i];
          if (accelMagBuffer[i] > maxVal) maxVal = accelMagBuffer[i];
          sumVal += accelMagBuffer[i];
          sumX   += accelXBuffer[i];
          sumY   += accelYBuffer[i];
          sumZ   += accelZBuffer[i];
        }

        float variation = maxVal - minVal;
        float avgAccel  = sumVal / bufferIndex;
        float avgX      = sumX / bufferIndex;
        float avgY      = sumY / bufferIndex;
        float avgZ      = sumZ / bufferIndex;

        // Compute Posture Vector Dot Product & Tilt Angle Shift
        float magBase = sqrt(snapshotBaselineAx * snapshotBaselineAx + snapshotBaselineAy * snapshotBaselineAy + snapshotBaselineAz * snapshotBaselineAz);
        float magPost = sqrt(avgX * avgX + avgY * avgY + avgZ * avgZ);

        float dotProduct = (snapshotBaselineAx * avgX + snapshotBaselineAy * avgY + snapshotBaselineAz * avgZ);
        float cosTilt = 1.0f;
        if (magBase > 0.1f && magPost > 0.1f) {
          cosTilt = dotProduct / (magBase * magPost);
        }
        if (cosTilt > 1.0f) cosTilt = 1.0f;
        if (cosTilt < -1.0f) cosTilt = -1.0f;

        float postureShiftDeg = acos(cosTilt) * (180.0f / PI);

        Serial.print("[FSM] Post-Impact Variation: ");
        Serial.print(variation, 2);
        Serial.print(" m/s^2 | Avg Magnitude: ");
        Serial.print(avgAccel, 2);
        Serial.print(" m/s^2 | Tilt Shift: ");
        Serial.print(postureShiftDeg, 1);
        Serial.println(" deg");

        bool stillnessConfirmed = (variation < INACTIVITY_VARIATION_MAX) && (avgAccel >= 7.0f && avgAccel <= 12.5f);
        bool postureShiftConfirmed = (cosTilt < POSTURE_TILT_COS_MAX);

        if (stillnessConfirmed && postureShiftConfirmed) {
          // Store captured telemetry
          capturedPostureChangeDeg = postureShiftDeg;
          capturedStillnessVariation = variation;

          // Calculate Dynamic Multi-Factor Fall Confidence Score (0.00 to 1.00)
          capturedFallConfidence = calculateFallConfidence(
            capturedImpactG,
            capturedRotationRads,
            capturedStillnessVariation,
            capturedPostureChangeDeg,
            capturedFreefallPresent
          );

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
          if (!stillnessConfirmed) Serial.println("[FSM] Event REJECTED: User continued moving.");
          if (!postureShiftConfirmed) Serial.println("[FSM] Event REJECTED: No significant posture orientation tilt shift.");
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