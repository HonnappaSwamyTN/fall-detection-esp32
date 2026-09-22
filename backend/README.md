# ESP32-C3 Fall Detection Backend API

Lightweight Python FastAPI backend that bridges the ESP32-C3 wearable fall detection device with Google Gemini AI analysis and Telegram Caregiver Notifications.

---

## Architecture Flow

```
+------------------+   HTTP POST /api/fall-event   +----------------------+
| ESP32-C3 Wearable| ----------------------------> | FastAPI Backend Server|
+------------------+  (Kinematic Telemetry JSON)   +----------------------+
                                                              |
                                           +------------------+------------------+
                                           |                                     |
                                           v                                     v
                            +-----------------------------+     +-------------------------------+
                            |   Google Gemini API         |     | Telegram Bot API              |
                            | (Kinematic Event Analysis)  |     | (Caregiver Notification Chat) |
                            +-----------------------------+     +-------------------------------+
```

---

## Setup & Installation

### 1. Install Dependencies

Ensure Python 3.10+ is installed, then run:

```bash
cd backend
pip install -r requirements.txt
```

### 2. Configure Environment Variables

Create a `.env` file inside the `backend/` folder based on `.env.example`:

```bash
cp .env.example .env
```

Edit `.env` and enter your API keys and credentials:

```ini
GEMINI_API_KEY=AIzaSy...
TELEGRAM_BOT_TOKEN=123456789:ABCdef...
TELEGRAM_CHAT_ID=123456789
```

> **Security Note**: Never commit `.env` to Git repository. `.env` is ignored by `.gitignore`.

---

## Running the Server

Start the FastAPI backend listening on all network interfaces (`0.0.0.0`):

```bash
cd backend
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

The API will be available at:
- **Health Check**: `http://localhost:8000/health`
- **Fall Event Endpoint**: `http://localhost:8000/api/fall-event`
- **Interactive API Docs (Swagger UI)**: `http://localhost:8000/docs`

---

## Connecting ESP32-C3 to the Backend

Follow these steps to link your physical ESP32-C3 wearable device to the backend API:

### 1. Find Your Computer's LAN IPv4 Address
On your PC hosting the FastAPI backend, run:
- **Windows (PowerShell/CMD)**: `ipconfig` (Look for `IPv4 Address`, e.g., `192.168.1.100`)
- **Linux/macOS (Terminal)**: `ifconfig` or `ip a` (Look for your Wi-Fi interface IP)

### 2. Ensure Network Connectivity
Ensure both your computer running FastAPI and the ESP32-C3 device are connected to the **same 2.4GHz Wi-Fi network**.

### 3. Configure Backend URL on ESP32-C3
You can set your backend URL (`http://<YOUR_PC_IP>:8000/api/fall-event`) using either method:
- **Option A (Default File)**: Edit `include/secrets.h`:
  ```cpp
  #define BACKEND_URL "http://192.168.1.100:8000/api/fall-event"
  ```
- **Option B (Provisioning Portal)**: Connect your phone/PC to the SoftAP `FallDetector-Setup` (IP `192.168.4.1`) and enter the URL in the `Backend API URL` field.

### 4. End-to-End ESP32 to FastAPI Integration Test
1. Power on the ESP32-C3 device.
2. Confirm over Serial Monitor (`115200` baud) that Wi-Fi connects and prints: `Target Backend API URL: http://<YOUR_PC_IP>:8000/api/fall-event`.
3. Simulate a fall event by dropping or rapidly tilting the device.
4. Observe the 10-second `PRE_ALERT` countdown (pulsing buzzer & LED). Do not press the cancel button.
5. Upon expiry, the ESP32 logs:
   ```
   [HTTP] Sending Fall Telemetry JSON to: http://192.168.1.100:8000/api/fall-event
   [HTTP] Backend Response Code: 201
   ```
6. Check your FastAPI console output and Telegram caregiver group chat for the incoming AI-analyzed alert!

---

## Local Testing Without ESP32

You can test the backend pipeline using the included Python test script or `curl`:

### Option A: Using `test_event.py`
In a separate terminal window, run:

```bash
python backend/test_event.py
```

### Option B: Using `curl`

```bash
curl -X POST "http://localhost:8000/api/fall-event" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32C3_TEST",
    "timestamp": "2026-09-23T00:25:00Z",
    "impact_g": 2.45,
    "rotation_rads": 3.12,
    "posture_change_deg": 68.5,
    "stillness_variation": 0.82,
    "fall_confidence": 0.95
  }'
```

---

## API Reference

### `GET /health`
Returns system status and configuration checks.

### `POST /api/fall-event`
Accepts hardware kinematic telemetry, performs Gemini AI assessment, dispatches Telegram caregiver alert, and returns the assessment result.
