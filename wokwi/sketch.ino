#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// =========================
// PIN CONFIGURATION
// =========================

#define SDA_PIN       4
#define SCL_PIN       5
#define BUZZER_PIN    6
#define LED_PIN       7
#define BUTTON_PIN    10

// =========================
// MPU6050
// =========================

#define MPU6050_ADDR  0x68

#define PWR_MGMT_1    0x6B
#define ACCEL_CONFIG  0x1C
#define GYRO_CONFIG   0x1B
#define ACCEL_XOUT_H  0x3B

// ±8g
#define ACCEL_SCALE   4096.0

// ±250 °/s
#define GYRO_SCALE    131.0

// =========================
// FALL DETECTION SETTINGS
// =========================

#define IMPACT_ACCEL_THRESHOLD     25.0
#define ROTATION_THRESHOLD         4.0
#define ROTATION_CHECK_MS          500
#define INACTIVITY_WINDOW_MS       2000
#define INACTIVITY_SAMPLE_MS       100
#define INACTIVITY_VARIATION_MAX   1.5

// =========================
// STATES
// =========================

enum FallState
{
    MONITORING,
    ROTATION_CHECK,
    INACTIVITY_CHECK,
    ALERTING,
    COOLDOWN
};

FallState currentState = MONITORING;

unsigned long stateStartTime = 0;
unsigned long lastSampleTime = 0;
unsigned long cooldownStartTime = 0;

// =========================
// SENSOR VARIABLES
// =========================

float accelX, accelY, accelZ;
float gyroX, gyroY, gyroZ;

float accelMagnitude;
float gyroMagnitude;

float inactivityMin;
float inactivityMax;
float inactivitySum;

int inactivitySamples;

// =========================
// WIFI / TELEGRAM
// =========================

String wifiSSID;
String wifiPassword;
String botToken;
String chatID;

bool wifiConnected = false;

// ======================================================
// SERIAL INPUT
// ======================================================

String readSerialInput(const char* message)
{
    Serial.println(message);

    while (!Serial.available())
    {
        delay(50);
    }

    String input = Serial.readStringUntil('\n');
    input.trim();

    return input;
}

// ======================================================
// MPU6050 FUNCTIONS
// ======================================================

void writeRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

bool readMPU6050()
{
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(ACCEL_XOUT_H);
    
    if (Wire.endTransmission(false) != 0)
        return false;

    Wire.requestFrom(MPU6050_ADDR, 14);

    if (Wire.available() < 14)
        return false;

    int16_t rawAx = (Wire.read() << 8) | Wire.read();
    int16_t rawAy = (Wire.read() << 8) | Wire.read();
    int16_t rawAz = (Wire.read() << 8) | Wire.read();

    Wire.read();
    Wire.read();

    int16_t rawGx = (Wire.read() << 8) | Wire.read();
    int16_t rawGy = (Wire.read() << 8) | Wire.read();
    int16_t rawGz = (Wire.read() << 8) | Wire.read();

    accelX = (rawAx / ACCEL_SCALE) * 9.80665;
    accelY = (rawAy / ACCEL_SCALE) * 9.80665;
    accelZ = (rawAz / ACCEL_SCALE) * 9.80665;

    gyroX = rawGx / GYRO_SCALE;
    gyroY = rawGy / GYRO_SCALE;
    gyroZ = rawGz / GYRO_SCALE;

    accelMagnitude = sqrt(
        accelX * accelX +
        accelY * accelY +
        accelZ * accelZ
    );

    gyroMagnitude = sqrt(
        gyroX * gyroX +
        gyroY * gyroY +
        gyroZ * gyroZ
    );

    return true;
}

// ======================================================
// MPU6050 INITIALIZATION
// ======================================================

bool initializeMPU()
{
    Wire.begin(SDA_PIN, SCL_PIN);

    delay(100);

    // Wake MPU6050
    writeRegister(PWR_MGMT_1, 0x00);

    // ±8g
    writeRegister(ACCEL_CONFIG, 0x10);

    // ±250 °/s
    writeRegister(GYRO_CONFIG, 0x00);

    delay(100);

    // Check WHO_AM_I
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(0x75);
    Wire.endTransmission(false);

    Wire.requestFrom(MPU6050_ADDR, 1);

    if (Wire.available())
    {
        uint8_t whoAmI = Wire.read();

        Serial.print("MPU6050 WHO_AM_I: 0x");
        Serial.println(whoAmI, HEX);

        return true;
    }

    return false;
}

// ======================================================
// WIFI
// ======================================================

void connectWiFi()
{
    Serial.println();
    Serial.println("Connecting to Wi-Fi...");

    WiFi.begin(
        wifiSSID.c_str(),
        wifiPassword.c_str()
    );

    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < 15000)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiConnected = true;

        Serial.println("Wi-Fi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        wifiConnected = false;

        Serial.println("Wi-Fi connection failed.");
        Serial.println("Fall detection will still work locally.");
    }
}

// ======================================================
// TELEGRAM
// ======================================================

void sendTelegramAlert()
{
    if (!wifiConnected)
    {
        Serial.println("Telegram alert skipped - Wi-Fi not connected.");
        return;
    }

    if (botToken.length() == 0 || chatID.length() == 0)
    {
        Serial.println("Telegram credentials are empty.");
        return;
    }

    WiFiClientSecure client;

    // Prototype/Wokwi only
    client.setInsecure();

    HTTPClient https;

    String url =
        "https://api.telegram.org/bot" +
        botToken +
        "/sendMessage";

    String message =
        "⚠️ FALL DETECTION ALERT!\n\n"
        "Possible fall detected.\n"
        "Please check the person.";

    String payload =
        "{\"chat_id\":\"" +
        chatID +
        "\",\"text\":\"" +
        message +
        "\"}";

    Serial.println("Sending Telegram alert...");

    if (https.begin(client, url))
    {
        https.addHeader("Content-Type", "application/json");

        int httpCode = https.POST(payload);

        Serial.print("Telegram HTTP code: ");
        Serial.println(httpCode);

        if (httpCode > 0)
        {
            Serial.println(https.getString());
        }

        https.end();
    }
    else
    {
        Serial.println("Could not connect to Telegram.");
    }
}

// ======================================================
// ALERT
// ======================================================

void triggerAlert()
{
    Serial.println();
    Serial.println("================================");
    Serial.println("      FALL DETECTED!");
    Serial.println("================================");

    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);

    sendTelegramAlert();

    stateStartTime = millis();
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println("   ESP32 FALL DETECTION SYSTEM");
    Serial.println("================================");
    Serial.println();

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    // --------------------------------
    // Get credentials from Serial
    // --------------------------------

    Serial.println("Enter the following information.");
    Serial.println("Press Enter after each value.");
    Serial.println();

    wifiSSID = readSerialInput(
        "Enter Wi-Fi SSID:"
    );

    wifiPassword = readSerialInput(
        "Enter Wi-Fi Password:"
    );

    botToken = readSerialInput(
        "Enter Telegram Bot Token:"
    );

    chatID = readSerialInput(
        "Enter Telegram Chat ID:"
    );

    Serial.println();
    Serial.println("Credentials received.");

    // --------------------------------
    // Initialize MPU6050
    // --------------------------------

    Serial.println();
    Serial.println("Initializing MPU6050...");

    if (initializeMPU())
    {
        Serial.println("MPU6050 initialized successfully.");
    }
    else
    {
        Serial.println("MPU6050 initialization failed!");
    }

    // --------------------------------
    // Wi-Fi
    // --------------------------------

    connectWiFi();

    Serial.println();
    Serial.println("System ready.");
    Serial.println();
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
    // ==========================================
    // Manual cancel button
    // ==========================================

    if (digitalRead(BUTTON_PIN) == LOW)
    {
        Serial.println("Button pressed - cancelling alert.");

        digitalWrite(BUZZER_PIN, LOW);
        digitalWrite(LED_PIN, LOW);

        currentState = MONITORING;

        delay(300);
        return;
    }

    // ==========================================
    // Read MPU6050
    // ==========================================

    if (!readMPU6050())
    {
        Serial.println("MPU6050 read failed.");
        delay(100);
        return;
    }

    // ==========================================
    // Detection state machine
    // ==========================================

    switch (currentState)
    {
        // --------------------------------------
        // MONITORING
        // --------------------------------------

        case MONITORING:

            if (accelMagnitude > IMPACT_ACCEL_THRESHOLD)
            {
                Serial.println();
                Serial.println("Impact detected!");

                Serial.print("Acceleration: ");
                Serial.print(accelMagnitude);
                Serial.println(" m/s²");

                stateStartTime = millis();

                currentState = ROTATION_CHECK;
            }

            break;

        // --------------------------------------
        // ROTATION CHECK
        // --------------------------------------

        case ROTATION_CHECK:

            if (gyroMagnitude > ROTATION_THRESHOLD)
            {
                Serial.println("Rotation detected!");

                inactivityMin = 1000;
                inactivityMax = -1000;
                inactivitySum = 0;

                inactivitySamples = 0;

                stateStartTime = millis();
                lastSampleTime = millis();

                currentState = INACTIVITY_CHECK;
            }
            else if (millis() - stateStartTime >
                     ROTATION_CHECK_MS)
            {
                Serial.println("Rotation not confirmed.");

                currentState = MONITORING;
            }

            break;

        // --------------------------------------
        // INACTIVITY CHECK
        // --------------------------------------

        case INACTIVITY_CHECK:

            if (millis() - lastSampleTime >=
                INACTIVITY_SAMPLE_MS)
            {
                lastSampleTime = millis();

                float a = accelMagnitude;

                if (a < inactivityMin)
                    inactivityMin = a;

                if (a > inactivityMax)
                    inactivityMax = a;

                inactivitySum += a;

                inactivitySamples++;

                Serial.print("Inactivity sample: ");
                Serial.println(a);
            }

            if (millis() - stateStartTime >=
                INACTIVITY_WINDOW_MS)
            {
                float variation =
                    inactivityMax - inactivityMin;

                float average =
                    inactivitySum / inactivitySamples;

                Serial.println();
                Serial.println("Inactivity analysis:");

                Serial.print("Average acceleration: ");
                Serial.println(average);

                Serial.print("Variation: ");
                Serial.println(variation);

                if (variation < INACTIVITY_VARIATION_MAX &&
                    average >= 7.0 &&
                    average <= 12.5)
                {
                    Serial.println("Possible fall confirmed!");

                    currentState = ALERTING;
                    stateStartTime = millis();
                }
                else
                {
                    Serial.println("Fall not confirmed.");

                    currentState = MONITORING;
                }
            }

            break;

        // --------------------------------------
        // ALERTING
        // --------------------------------------

        case ALERTING:

            triggerAlert();

            currentState = COOLDOWN;

            break;

        // --------------------------------------
        // COOLDOWN
        // --------------------------------------

        case COOLDOWN:

            if (millis() - stateStartTime >= 5000)
            {
                digitalWrite(BUZZER_PIN, LOW);
                digitalWrite(LED_PIN, LOW);

                Serial.println("Alert ended.");

                cooldownStartTime = millis();

                currentState = COOLDOWN;
            }

            if (millis() - cooldownStartTime >= 10000)
            {
                Serial.println("Returning to monitoring.");

                currentState = MONITORING;
            }

            break;
    }

    delay(10);
}