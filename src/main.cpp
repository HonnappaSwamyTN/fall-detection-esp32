/*
  Fall Detection & Auto-Alert System
  ESP32-C3 + MPU6050

  Features:
  - Raw I2C communication with MPU6050
  - Impact detection
  - Rotation detection
  - Post-impact inactivity detection
  - Buzzer + LED alert
  - Manual cancel button
  - Telegram alert over Wi-Fi
  - I2C error checking
  - MPU6050 WHO_AM_I verification
  - Fall cooldown
  - Optional sensor debugging

  PIN MAP:
    MPU6050 SDA -> GPIO4
    MPU6050 SCL -> GPIO5
    Buzzer      -> GPIO6
    LED         -> GPIO7 (through 220 ohm resistor)
    Button      -> GPIO10
    Button other leg -> GND

  IMPORTANT:
    Calibrate thresholds using actual sensor data.
*/


#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>


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


// ============================================================
// DYNAMIC CREDENTIALS & PREFERENCES
// ============================================================

Preferences preferences;

String wifiSSID     = "";
String wifiPassword = "";
String botToken     = "";
String chatID       = "";


// ============================================================
// DEBUG
// ============================================================

// 1 = print acceleration and gyro values
// 0 = disable sensor debugging

#define DEBUG_SENSOR 1


// ============================================================
// MPU6050 SCALE
// ============================================================

// Accelerometer: ±8g
// 4096 LSB = 1g

#define ACCEL_SCALE 4096.0

// Gyroscope: ±250 degrees/second
// 131 LSB = 1 degree/second

#define GYRO_SCALE 131.0


// ============================================================
// FALL DETECTION THRESHOLDS
// ============================================================

// IMPORTANT:
// These are starting values.
// They MUST be calibrated using real sensor data.

#define IMPACT_ACCEL_THRESHOLD     25.0

// rad/s
#define ROTATION_THRESHOLD         4.0

// Time after impact during which rotation is checked
#define ROTATION_CHECK_MS          500


// How long post-impact movement is monitored
#define INACTIVITY_WINDOW_MS       2000

// Sampling interval
#define INACTIVITY_SAMPLE_MS       100

// Maximum acceleration variation considered "still"
#define INACTIVITY_VARIATION_MAX   1.5


// ============================================================
// ALERT SETTINGS
// ============================================================

// Buzzer + LED duration
#define ALERT_HOLD_MS              5000

// Prevent repeated detection immediately after an alert
#define FALL_COOLDOWN_MS           10000


// ============================================================
// INACTIVITY BUFFER
// ============================================================

#define BUFFER_SIZE 20

float accelBuffer[BUFFER_SIZE];

int bufferIndex = 0;

unsigned long lastSampleTime = 0;


// ============================================================
// SYSTEM STATE
// ============================================================

enum SystemState
{
  MONITORING,
  ROTATION_CHECK,
  INACTIVITY_CHECK,
  ALERTING,
  COOLDOWN
};

SystemState currentState = MONITORING;


// ============================================================
// STATE VARIABLES
// ============================================================

unsigned long stateStartTime = 0;

bool rotationConfirmed = false;

bool alertSent = false;


// ============================================================
// MPU6050 REGISTER WRITE
// ============================================================

bool mpuWriteRegister(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(MPU_ADDR);

  Wire.write(reg);
  Wire.write(value);

  uint8_t error = Wire.endTransmission();

  if (error != 0)
  {
    Serial.print("I2C write error: ");
    Serial.println(error);

    return false;
  }

  return true;
}


// ============================================================
// MPU6050 REGISTER READ
// ============================================================

bool mpuReadRegister(uint8_t reg, uint8_t &value)
{
  Wire.beginTransmission(MPU_ADDR);

  Wire.write(reg);

  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }

  uint8_t received = Wire.requestFrom(MPU_ADDR, (uint8_t)1, true);

  if (received != 1 || !Wire.available())
  {
    return false;
  }

  value = Wire.read();

  return true;
}


// ============================================================
// CHECK MPU6050 PRESENCE
// ============================================================

bool mpuCheckPresent()
{
  Wire.beginTransmission(MPU_ADDR);

  return (Wire.endTransmission() == 0);
}


// ============================================================
// READ MPU6050 ACCEL + GYRO
// ============================================================

bool mpuReadAccelGyro(
  float &ax,
  float &ay,
  float &az,
  float &gx,
  float &gy,
  float &gz
)
{
  Wire.beginTransmission(MPU_ADDR);

  Wire.write(ACCEL_XOUT_H);

  // Repeated start
  if (Wire.endTransmission(false) != 0)
  {
    Serial.println("MPU6050 I2C transmission failed.");

    return false;
  }


  // Accelerometer = 6 bytes
  // Temperature   = 2 bytes
  // Gyroscope     = 6 bytes
  // Total         = 14 bytes

  uint8_t received = Wire.requestFrom(
    MPU_ADDR,
    (uint8_t)14,
    true
  );


  if (received != 14)
  {
    Serial.print("MPU6050 read error. Bytes received: ");
    Serial.println(received);

    return false;
  }


  int16_t rawAx =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawAy =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawAz =
    ((int16_t)Wire.read() << 8) | Wire.read();


  // Skip temperature
  Wire.read();
  Wire.read();


  int16_t rawGx =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawGy =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawGz =
    ((int16_t)Wire.read() << 8) | Wire.read();


  // ==========================================================
  // CONVERT RAW VALUES
  // ==========================================================

  // Acceleration -> m/s²

  ax = (rawAx / ACCEL_SCALE) * 9.80665;
  ay = (rawAy / ACCEL_SCALE) * 9.80665;
  az = (rawAz / ACCEL_SCALE) * 9.80665;


  // Gyroscope:
  // deg/s -> rad/s

  gx = (rawGx / GYRO_SCALE) * (PI / 180.0);
  gy = (rawGy / GYRO_SCALE) * (PI / 180.0);
  gz = (rawGz / GYRO_SCALE) * (PI / 180.0);


  return true;
}


// ============================================================
// PROTOTYPES
// ============================================================

void resetToMonitoring();
void connectWiFi();
void maintainWiFiConnection();
void sendTelegramAlert();
bool loadCredentials();
void saveCredentials(const String& ssid, const String& pass, const String& token, const String& chat);
void clearCredentials();
void startProvisioningPortal();


// ============================================================
// PREFERENCES & PROVISIONING MANAGER
// ============================================================

bool loadCredentials()
{
  preferences.begin("fall_config", true);
  wifiSSID     = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  botToken     = preferences.getString("bot_token", "");
  chatID       = preferences.getString("chat_id", "");
  preferences.end();

  return (wifiSSID.length() > 0 && botToken.length() > 0 && chatID.length() > 0);
}

void saveCredentials(const String& ssid, const String& pass, const String& token, const String& chat)
{
  preferences.begin("fall_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.putString("bot_token", token);
  preferences.putString("chat_id", chat);
  preferences.end();
}

void clearCredentials()
{
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
      
      <label for="token">Telegram Bot Token</label>
      <input type="text" id="token" name="token" required placeholder="123456789:ABCdef...">
      
      <label for="chat">Telegram Chat ID</label>
      <input type="text" id="chat" name="chat" required placeholder="123456789">
      
      <input type="submit" value="Save & Restart">
    </form>
    <div class="note">Device will reboot automatically after saving.</div>
  </div>
</body>
</html>
)rawliteral";

void handleRoot()
{
  webServer.send(200, "text/html", SETUP_HTML);
}

void handleSave()
{
  if (webServer.hasArg("ssid") && webServer.hasArg("token") && webServer.hasArg("chat"))
  {
    String reqSSID  = webServer.arg("ssid");
    String reqPass  = webServer.arg("pass");
    String reqToken = webServer.arg("token");
    String reqChat  = webServer.arg("chat");

    reqSSID.trim();
    reqPass.trim();
    reqToken.trim();
    reqChat.trim();

    saveCredentials(reqSSID, reqPass, reqToken, reqChat);

    String resp = "<html><body style='font-family:sans-serif;background:#121212;color:#00e676;text-align:center;padding:50px;'>";
    resp += "<h2>Settings Saved Successfully!</h2><p style='color:#fff;'>Device is restarting and connecting to Wi-Fi...</p></body></html>";

    webServer.send(200, "text/html", resp);
    delay(2000);
    ESP.restart();
  }
  else
  {
    webServer.send(400, "text/plain", "Bad Request: Missing required fields");
  }
}

void startProvisioningPortal()
{
  Serial.println();
  Serial.println("==========================================");
  Serial.println("   STARTING PROVISIONING SETUP PORTAL   ");
  Serial.println("==========================================");
  Serial.println("Broadcasting SoftAP: FallDetector-Setup");
  Serial.println("Connect your phone/PC to 'FallDetector-Setup'");
  Serial.println("Open browser at http://192.168.4.1 if portal does not open automatically.");
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

  while (true)
  {
    dnsServer.processNextRequest();
    webServer.handleClient();

    if (millis() - lastBlink > 500)
    {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }

    delay(5);
  }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);


  // ==========================================================
  // GPIO SETUP
  // ==========================================================

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(LED_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);


  digitalWrite(BUZZER_PIN, LOW);

  digitalWrite(LED_PIN, LOW);


  Serial.println();
  Serial.println("==============================");
  Serial.println("Fall Detection System");
  Serial.println("==============================");

  // ==========================================================
  // CHECK RESET BUTTON ON BOOT
  // ==========================================================
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    Serial.println("Reset button pressed during startup.");
    Serial.println("Hold button for 3 seconds to clear saved credentials...");

    unsigned long pressStart = millis();
    bool resetTriggered = false;

    while (digitalRead(BUTTON_PIN) == LOW)
    {
      if (millis() - pressStart >= 3000)
      {
        resetTriggered = true;
        break;
      }
      delay(50);
    }

    if (resetTriggered)
    {
      Serial.println("Reset confirmed! Clearing credentials and starting setup portal...");

      for (int i = 0; i < 10; i++)
      {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
      }
      digitalWrite(LED_PIN, LOW);

      clearCredentials();
      startProvisioningPortal();
    }
    else
    {
      Serial.println("Button released early. Continuing normal startup.");
    }
  }

  // ==========================================================
  // LOAD SAVED CREDENTIALS
  // ==========================================================
  if (!loadCredentials())
  {
    Serial.println("No saved credentials found in NVS.");
    Serial.println("Launching setup portal...");
    startProvisioningPortal();
  }

  Serial.println("Credentials loaded from NVS successfully.");

  // ==========================================================
  // START I2C
  // ==========================================================

  Serial.println("Initializing I2C...");

  Wire.begin(SDA_PIN, SCL_PIN);

  Wire.setClock(400000);

  delay(100);


  // ==========================================================
  // CHECK MPU6050
  // ==========================================================

  Serial.println("Checking MPU6050...");


  if (!mpuCheckPresent())
  {
    Serial.println();
    Serial.println("ERROR: MPU6050 not found.");
    Serial.println("Check:");
    Serial.println("SDA -> GPIO4");
    Serial.println("SCL -> GPIO5");
    Serial.println("VCC -> 3.3V");
    Serial.println("GND -> GND");

    while (1)
    {
      delay(1000);
    }
  }


  Serial.println("MPU6050 I2C device detected.");


  // ==========================================================
  // WHO_AM_I CHECK
  // ==========================================================

  uint8_t whoAmI = 0;

  if (!mpuReadRegister(WHO_AM_I_REG, whoAmI))
  {
    Serial.println("ERROR: Could not read WHO_AM_I.");

    while (1)
    {
      delay(1000);
    }
  }


  Serial.print("MPU6050 WHO_AM_I = 0x");

  Serial.println(whoAmI, HEX);


  if (whoAmI != 0x68)
  {
    Serial.println("ERROR: Device identity does not match MPU6050.");

    while (1)
    {
      delay(1000);
    }
  }


  Serial.println("MPU6050 identity verified.");


  // ==========================================================
  // WAKE MPU6050
  // ==========================================================

  Serial.println("Waking MPU6050...");

  if (!mpuWriteRegister(PWR_MGMT_1, 0x00))
  {
    Serial.println("ERROR: Could not wake MPU6050.");

    while (1)
    {
      delay(1000);
    }
  }

  delay(100);


  // ==========================================================
  // ACCELEROMETER CONFIGURATION
  // ==========================================================

  // 0x10 = ±8g (allows measuring impacts up to 78.4 m/s² without saturation)

  if (!mpuWriteRegister(ACCEL_CONFIG, 0x10))
  {
    Serial.println("WARNING: Accelerometer configuration failed.");
  }


  // ==========================================================
  // GYROSCOPE CONFIGURATION
  // ==========================================================

  // 0x00 = ±250 deg/s

  if (!mpuWriteRegister(GYRO_CONFIG, 0x00))
  {
    Serial.println("WARNING: Gyroscope configuration failed.");
  }


  Serial.println("MPU6050 configured successfully.");


  // ==========================================================
  // CONNECT WIFI
  // ==========================================================

  connectWiFi();


  Serial.println();
  Serial.println("==============================");
  Serial.println("System ready.");
  Serial.println("Monitoring for falls...");
  Serial.println("==============================");
  Serial.println();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  // Non-blocking background Wi-Fi auto-recovery
  maintainWiFiConnection();

  // ==========================================================
  // READ SENSOR
  // ==========================================================

  float ax, ay, az;
  float gx, gy, gz;


  bool sensorOK =
    mpuReadAccelGyro(
      ax,
      ay,
      az,
      gx,
      gy,
      gz
    );


  // If sensor read failed, don't process garbage values.

  if (!sensorOK)
  {
    delay(100);

    return;
  }


  // ==========================================================
  // CALCULATE MAGNITUDES
  // ==========================================================

  float accelMag = sqrt(
    ax * ax +
    ay * ay +
    az * az
  );


  float gyroMag = sqrt(
    gx * gx +
    gy * gy +
    gz * gz
  );


  // ==========================================================
  // DEBUG OUTPUT
  // ==========================================================

#if DEBUG_SENSOR

  Serial.print("Acceleration: ");

  Serial.print(accelMag, 2);

  Serial.print(" m/s^2 | Gyro: ");

  Serial.print(gyroMag, 2);

  Serial.println(" rad/s");

#endif


  // ==========================================================
  // MANUAL CANCEL BUTTON
  // ==========================================================

  if (digitalRead(BUTTON_PIN) == LOW)
  {
    Serial.println("Manual cancel pressed.");

    resetToMonitoring();

    delay(300);
  }


  // ==========================================================
  // STATE MACHINE
  // ==========================================================

  switch (currentState)
  {

    // ========================================================
    // MONITORING
    // ========================================================

    case MONITORING:

      if (accelMag > IMPACT_ACCEL_THRESHOLD)
      {
        Serial.println();
        Serial.println("!!! IMPACT DETECTED !!!");

        Serial.println(
          "Checking for rotation..."
        );


        currentState = ROTATION_CHECK;

        stateStartTime = millis();

        // Check if rotation signature is already present at the moment of impact
        rotationConfirmed = (gyroMag > ROTATION_THRESHOLD);
      }

      break;


    // ========================================================
    // ROTATION CHECK
    // ========================================================

    case ROTATION_CHECK:

      if (gyroMag > ROTATION_THRESHOLD)
      {
        rotationConfirmed = true;

        Serial.println(
          "Rotation signature detected."
        );
      }


      if (
        millis() - stateStartTime
        >= ROTATION_CHECK_MS
      )
      {

        if (rotationConfirmed)
        {
          Serial.println(
            "Rotation confirmed."
          );

          Serial.println(
            "Checking post-impact inactivity..."
          );


          currentState = INACTIVITY_CHECK;

          stateStartTime = millis();

          bufferIndex = 0;

          lastSampleTime = millis();
        }
        else
        {
          Serial.println(
            "No rotation detected."
          );

          Serial.println(
            "Event rejected."
          );


          currentState = MONITORING;
        }
      }

      break;


    // ========================================================
    // INACTIVITY CHECK
    // ========================================================

    case INACTIVITY_CHECK:

      if (
        millis() - lastSampleTime
        >= INACTIVITY_SAMPLE_MS
      )
      {

        if (bufferIndex < BUFFER_SIZE)
        {
          accelBuffer[bufferIndex] = accelMag;

          bufferIndex++;

          lastSampleTime = millis();
        }
      }


      // Wait until:
      // 1. 2 seconds have passed
      // OR
      // 2. Buffer is full

      if (
        (
          millis() - stateStartTime
          >= INACTIVITY_WINDOW_MS
        )
        ||
        (
          bufferIndex >= BUFFER_SIZE
        )
      )
      {

        // Safety check

        if (bufferIndex == 0)
        {
          Serial.println(
            "No inactivity samples available."
          );

          currentState = MONITORING;

          break;
        }


        // ====================================================
        // CALCULATE MIN, MAX, AND MEAN ACCELERATION
        // ====================================================

        float minVal = accelBuffer[0];
        float maxVal = accelBuffer[0];
        float sumVal = accelBuffer[0];


        for (int i = 1; i < bufferIndex; i++)
        {

          if (accelBuffer[i] < minVal)
          {
            minVal = accelBuffer[i];
          }


          if (accelBuffer[i] > maxVal)
          {
            maxVal = accelBuffer[i];
          }

          sumVal += accelBuffer[i];
        }


        float variation = maxVal - minVal;
        float avgAccel  = sumVal / bufferIndex;


        Serial.print(
          "Post-impact variation: "
        );

        Serial.print(
          variation,
          2
        );

        Serial.print(
          " m/s^2 | Avg acceleration: "
        );

        Serial.print(
          avgAccel,
          2
        );

        Serial.println(" m/s^2");


        // ====================================================
        // FALL CONFIRMED
        // Stillness confirmed if movement variation is low AND
        // average acceleration magnitude is near 1g gravity (7.0 to 12.5 m/s²)
        // ====================================================

        if (
          (variation < INACTIVITY_VARIATION_MAX)
          &&
          (avgAccel >= 7.0f && avgAccel <= 12.5f)
        )
        {

          Serial.println();
          Serial.println(
            "================================"
          );

          Serial.println(
            "      FALL DETECTED!"
          );

          Serial.println(
            "================================"
          );


          currentState = ALERTING;

          stateStartTime = millis();

          alertSent = false;
        }
        else
        {

          Serial.println(
            "Movement continued or unstable resting position."
          );

          Serial.println(
            "Fall rejected."
          );


          currentState = MONITORING;
        }
      }

      break;


    // ========================================================
    // ALERTING
    // ========================================================

    case ALERTING:

      digitalWrite(
        BUZZER_PIN,
        HIGH
      );

      digitalWrite(
        LED_PIN,
        HIGH
      );


      // Send Telegram only once

      if (!alertSent)
      {
        sendTelegramAlert();

        alertSent = true;
      }


      // Keep local alarm active

      if (
        millis() - stateStartTime
        >= ALERT_HOLD_MS
      )
      {

        Serial.println(
          "Alert period finished."
        );

        Serial.println(
          "Entering cooldown..."
        );


        digitalWrite(
          BUZZER_PIN,
          LOW
        );

        digitalWrite(
          LED_PIN,
          LOW
        );


        currentState = COOLDOWN;

        stateStartTime = millis();
      }

      break;


    // ========================================================
    // COOLDOWN
    // ========================================================

    case COOLDOWN:

      // Ignore new fall detections temporarily

      if (
        millis() - stateStartTime
        >= FALL_COOLDOWN_MS
      )
      {

        Serial.println(
          "Cooldown finished."
        );

        Serial.println(
          "Returning to monitoring."
        );


        resetToMonitoring();
      }

      break;
  }


  // Small loop delay

  delay(50);
}


// ============================================================
// RESET SYSTEM
// ============================================================

void resetToMonitoring()
{
  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  digitalWrite(
    LED_PIN,
    LOW
  );


  currentState = MONITORING;

  rotationConfirmed = false;

  alertSent = false;

  bufferIndex = 0;
}


// ============================================================
// WIFI CONNECTION
// ============================================================

void connectWiFi()
{
  Serial.println();

  Serial.print(
    "Connecting to Wi-Fi network: "
  );
  Serial.println(wifiSSID);


  WiFi.mode(WIFI_STA);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );


  int attempts = 0;


  while (
    WiFi.status() != WL_CONNECTED
    &&
    attempts < 30
  )
  {

    delay(500);

    Serial.print(".");

    attempts++;
  }


  Serial.println();


  if (
    WiFi.status() == WL_CONNECTED
  )
  {

    Serial.println(
      "Wi-Fi connected successfully."
    );


    Serial.print(
      "IP address: "
    );

    Serial.println(
      WiFi.localIP()
    );
  }
  else
  {

    Serial.println(
      "Wi-Fi connection failed or timed out."
    );

    Serial.println(
      "Fall detection will operate LOCALLY (Buzzer & LED active)."
    );

    Serial.println(
      "Telegram alerts will be unavailable."
    );
  }
}


// ============================================================
// NON-BLOCKING WIFI RECOVERY
// ============================================================

unsigned long lastWiFiReconnectAttempt = 0;

void maintainWiFiConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - lastWiFiReconnectAttempt >= 30000)
    {
      lastWiFiReconnectAttempt = millis();
      Serial.println("Wi-Fi disconnected. Triggering background reconnect...");
      WiFi.reconnect();
    }
  }
}


// ============================================================
// TELEGRAM ALERT
// ============================================================

void sendTelegramAlert()
{
  // ==========================================================
  // CHECK WIFI
  // ==========================================================

  if (
    WiFi.status() != WL_CONNECTED
  )
  {

    Serial.println(
      "Telegram alert skipped."
    );

    Serial.println(
      "Wi-Fi is not connected."
    );

    return;
  }


  Serial.println(
    "Sending Telegram alert..."
  );


  // ==========================================================
  // SECURE CLIENT
  // ==========================================================

  WiFiClientSecure client;

  // For prototype/testing.
  // PRODUCTION SECURITY NOTE: For production deployment, replace client.setInsecure()
  // with Telegram root CA certificate validation: client.setCACert(TELEGRAM_ROOT_CA_CERT)
  // to prevent Man-in-the-Middle (MITM) network attacks.

  client.setInsecure();


  HTTPClient https;


  // ==========================================================
  // TELEGRAM API URL
  // ==========================================================

  String url =
    "https://api.telegram.org/bot"
    + botToken
    + "/sendMessage";


  // ==========================================================
  // START HTTPS
  // ==========================================================

  if (!https.begin(client, url))
  {

    Serial.println(
      "Unable to connect to Telegram API."
    );

    return;
  }


  // ==========================================================
  // HTTP HEADER
  // ==========================================================

  https.addHeader(
    "Content-Type",
    "application/json"
  );


  // ==========================================================
  // JSON MESSAGE
  // ==========================================================

  String payload =
    "{\"chat_id\":\""
    + chatID
    + "\",\"text\":\""
    + "FALL DETECTED! Immediate attention may be required."
    + "\"}";


  // ==========================================================
  // SEND POST
  // ==========================================================

  int responseCode =
    https.POST(payload);


  // ==========================================================
  // CHECK RESPONSE
  // ==========================================================

  if (responseCode > 0)
  {

    Serial.print(
      "Telegram response code: "
    );

    Serial.println(
      responseCode
    );


    String response =
      https.getString();


    Serial.println(
      "Telegram response:"
    );

    Serial.println(
      response
    );


    if (
      responseCode == 200
    )
    {

      Serial.println(
        "Telegram alert sent successfully."
      );
    }
    else
    {

      Serial.println(
        "Telegram returned an error."
      );
    }
  }
  else
  {

    Serial.print(
      "Telegram send failed: "
    );

    Serial.println(
      https.errorToString(
        responseCode
      )
    );
  }


  https.end();
}