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
#include <secrets.h>


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
// DEBUG
// ============================================================

// 1 = print acceleration and gyro values
// 0 = disable sensor debugging

#define DEBUG_SENSOR 1


// ============================================================
// MPU6050 SCALE
// ============================================================

// Accelerometer: ±2g
// 16384 LSB = 1g

#define ACCEL_SCALE 16384.0

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


  // ==========================================================
  // START I2C
  // ==========================================================

  Serial.println();
  Serial.println("==============================");
  Serial.println("Fall Detection System");
  Serial.println("==============================");

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

  // 0x00 = ±2g

  if (!mpuWriteRegister(ACCEL_CONFIG, 0x00))
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

        rotationConfirmed = false;
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
        // FIND MIN/MAX
        // ====================================================

        float minVal = accelBuffer[0];

        float maxVal = accelBuffer[0];


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
        }


        float variation =
          maxVal - minVal;


        Serial.print(
          "Post-impact acceleration variation: "
        );

        Serial.print(
          variation,
          2
        );

        Serial.println(" m/s^2");


        // ====================================================
        // FALL CONFIRMED
        // ====================================================

        if (
          variation <
          INACTIVITY_VARIATION_MAX
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
            "Movement continued."
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
    "Connecting to Wi-Fi"
  );


  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  int attempts = 0;


  while (
    WiFi.status() != WL_CONNECTED
    &&
    attempts < 20
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
      "Wi-Fi connected."
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
      "Wi-Fi connection failed."
    );

    Serial.println(
      "Fall detection will still work."
    );

    Serial.println(
      "Telegram alerts will be unavailable."
    );
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
  // Production version should use certificate validation.

  client.setInsecure();


  HTTPClient https;


  // ==========================================================
  // TELEGRAM API URL
  // ==========================================================

  String url =
    "https://api.telegram.org/bot"
    + String(BOT_TOKEN)
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
    + String(CHAT_ID)
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