# ESP32-C3 Fall Detection System

Wearable fall-detection prototype using ESP32-C3 and MPU6050.

## Hardware

- ESP32-C3
- MPU6050
- Buzzer
- LED + 220Ω resistor
- Push button

## Pin Connections

| Component | ESP32-C3 |
|---|---|
| MPU6050 SDA | GPIO 4 |
| MPU6050 SCL | GPIO 5 |
| Buzzer | GPIO 6 |
| LED | GPIO 7 |
| Button | GPIO 10 |

## Setup

1. Clone the repository.
2. Open the project in PlatformIO/Antigravity.
3. Connect the ESP32-C3.
4. Build and upload the firmware.
5. Configure Wi-Fi and Telegram through the `FallDetector-Setup` portal.
6. Open `192.168.4.1` to enter the credentials.

## Detection Logic

Impact → Rotation → Inactivity → Alert

If a potential fall is detected, the buzzer and LED activate and a Telegram notification is sent when Wi-Fi is available.

## Project Status

Currently under development and testing.
