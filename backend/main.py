import os
import json
import logging
import datetime
from typing import Dict, Any, Optional, List
from fastapi import FastAPI, HTTPException, status, Query
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
import httpx
from dotenv import load_dotenv
from google import genai
from google.genai import types

import database

# Configure Logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)

# Load Environment Variables from .env file
load_dotenv()

# Initialize SQLite Database on startup
database.init_db()

app = FastAPI(
    title="ESP32-C3 Fall Detection AI Backend",
    description="Lightweight backend service integrating SQLite Event Storage, Gemini AI analysis, and Telegram Caregiver Alerts",
    version="2.0.0"
)

# ============================================================
# SYSTEM STATUS IN-MEMORY TRACKER
# ============================================================
class SystemState:
    def __init__(self):
        self.gemini_last_status: str = "configured"  # not_configured, configured, working, error
        self.gemini_last_error: Optional[str] = None
        self.gemini_last_success_time: Optional[str] = None
        
        self.telegram_last_status: str = "configured"  # not_configured, configured, delivered, failed
        self.telegram_last_error: Optional[str] = None
        self.telegram_last_success_time: Optional[str] = None

        self.devices: Dict[str, Dict[str, Any]] = {}

    def update_device_heartbeat(self, device_id: str, uptime_seconds: Optional[int], wifi_rssi: Optional[int]):
        now = datetime.datetime.now(datetime.timezone.utc)
        self.devices[device_id] = {
            "last_seen_dt": now,
            "uptime_seconds": uptime_seconds,
            "wifi_rssi": wifi_rssi
        }

    def get_device_status(self, device_id: str = "ESP32C3_01") -> Dict[str, Any]:
        dev_info = self.devices.get(device_id)
        if not dev_info:
            return {
                "device_id": device_id,
                "status": "never_connected",
                "last_seen": None,
                "seconds_since_last_seen": None,
                "wifi_rssi": None,
                "uptime_seconds": None
            }
        
        now = datetime.datetime.now(datetime.timezone.utc)
        last_seen_dt = dev_info["last_seen_dt"]
        diff_seconds = max(0, int((now - last_seen_dt).total_seconds()))

        status_str = "online" if diff_seconds <= 30 else "offline"
        return {
            "device_id": device_id,
            "status": status_str,
            "last_seen": last_seen_dt.isoformat(),
            "seconds_since_last_seen": diff_seconds,
            "wifi_rssi": dev_info.get("wifi_rssi"),
            "uptime_seconds": dev_info.get("uptime_seconds")
        }

system_state = SystemState()

def get_gemini_config_status() -> tuple[bool, str]:
    api_key = os.getenv("GEMINI_API_KEY", "").strip()
    if not api_key or api_key.startswith("your_"):
        return False, "GEMINI_API_KEY is not configured"
    return True, ""

def get_telegram_config_status() -> tuple[bool, str]:
    bot_token = os.getenv("TELEGRAM_BOT_TOKEN", "").strip()
    chat_id = os.getenv("TELEGRAM_CHAT_ID", "").strip()
    if not bot_token or bot_token.startswith("your_"):
        return False, "TELEGRAM_BOT_TOKEN is not configured"
    if not chat_id or chat_id.startswith("your_"):
        return False, "TELEGRAM_CHAT_ID is not configured"
    return True, ""

def sanitize_secret(msg: str, bot_token: Optional[str] = None) -> str:
    if not msg:
        return ""
    if bot_token and len(bot_token) > 5 and bot_token in msg:
        msg = msg.replace(bot_token, "[MASKED_BOT_TOKEN]")
    env_token = os.getenv("TELEGRAM_BOT_TOKEN")
    if env_token and len(env_token) > 5 and env_token in msg:
        msg = msg.replace(env_token, "[MASKED_BOT_TOKEN]")
    env_gemini = os.getenv("GEMINI_API_KEY")
    if env_gemini and len(env_gemini) > 5 and env_gemini in msg:
        msg = msg.replace(env_gemini, "[MASKED_GEMINI_KEY]")
    return msg

def parse_safe_gemini_error(exc: Exception) -> str:
    if exc is None:
        return "Unknown Gemini API error"
    
    err_str = sanitize_secret(str(exc))
    err_type = type(exc).__name__

    if "401" in err_str or "API_KEY_INVALID" in err_str or "API key not valid" in err_str or "unauthorized" in err_str.lower():
        return "Authentication failed (Invalid API key)"
    if "429" in err_str or "RESOURCE_EXHAUSTED" in err_str or "quota" in err_str.lower():
        return "API quota exceeded"
    if "404" in err_str or "NOT_FOUND" in err_str or "not found" in err_str.lower():
        return "Model unavailable"
    if "timeout" in err_str.lower() or "connect" in err_str.lower() or "deadline" in err_str.lower():
        return "Request timed out"
    if "400" in err_str or "INVALID_ARGUMENT" in err_str:
        return "Invalid request format"
    
    first_line = err_str.split("\n")[0]
    if len(first_line) > 120:
        first_line = first_line[:117] + "..."
    return f"Gemini API error ({err_type}: {first_line})"

# ============================================================
# PYDANTIC SCHEMAS
# ============================================================
class HeartbeatPayload(BaseModel):
    device_id: str = Field(default="ESP32C3_01", description="Unique identifier of the ESP32-C3 wearable device")
    timestamp: Optional[str] = Field(default=None, description="Optional device uptime or timestamp string")
    uptime_seconds: Optional[int] = Field(default=None, ge=0, description="Device uptime in seconds")
    wifi_rssi: Optional[int] = Field(default=None, description="Wi-Fi signal RSSI in dBm")

class DeviceStatusResponse(BaseModel):
    device_id: str
    status: str
    last_seen: Optional[str] = None
    seconds_since_last_seen: Optional[int] = None
    wifi_rssi: Optional[int] = None
    uptime_seconds: Optional[int] = None

class FallEventRequest(BaseModel):
    device_id: str = Field(..., description="Unique identifier of the ESP32-C3 wearable device", example="ESP32C3_01")
    timestamp: str = Field(..., description="ISO 8601 timestamp or device time string", example="2026-09-23T00:20:00Z")
    impact_g: float = Field(..., ge=0.0, description="Peak impact acceleration magnitude in g", example=2.45)
    rotation_rads: float = Field(..., ge=0.0, description="Angular velocity magnitude in rad/s", example=3.12)
    posture_change_deg: float = Field(..., ge=0.0, le=180.0, description="Orientation tilt angle shift in degrees", example=68.5)
    stillness_variation: float = Field(..., ge=0.0, description="Post-impact acceleration variance in m/s^2", example=0.82)
    fall_confidence: float = Field(default=0.9, ge=0.0, le=1.0, description="Algorithmic fall confidence score (0.0 - 1.0)", example=0.95)


class GeminiAssessmentSchema(BaseModel):
    severity: str = Field(..., description="Severity classification: LOW, MEDIUM, HIGH, or CRITICAL")
    assessment: str = Field(..., description="Concise kinematic assessment of the fall event")
    caregiver_message: str = Field(..., description="Clear urgent message formatted for the caregiver")
    recommended_action: str = Field(..., description="Immediate recommended response action")

class FallEventResponse(BaseModel):
    event_id: str
    status: str
    telegram_delivered: bool
    gemini_assessment: Dict[str, Any]

class EventHistoryResponse(BaseModel):
    events: List[Dict[str, Any]]

# ============================================================
# GEMINI API HELPER
# ============================================================
def generate_fallback_assessment(event: FallEventRequest, reason: str) -> Dict[str, Any]:
    severity = "HIGH" if event.impact_g > 2.0 or event.stillness_variation < 1.0 else "MEDIUM"
    return {
        "severity": severity,
        "assessment": f"Sensor-confirmed fall event (Impact: {event.impact_g:.2f}g, Posture Tilt: {event.posture_change_deg:.1f}°). Fallback assessment used ({reason}).",
        "caregiver_message": "URGENT ALERT: A potential fall was detected by the wearable device. Please check on the wearer immediately!",
        "recommended_action": "Attempt to contact the wearer or visit their location immediately to verify safety."
    }

def analyze_fall_with_gemini(event: FallEventRequest, override_api_key: Optional[str] = None) -> Dict[str, Any]:
    api_key = override_api_key if override_api_key is not None else os.getenv("GEMINI_API_KEY")
    
    if not api_key or api_key.startswith("your_"):
        reason = "GEMINI_API_KEY is not configured"
        logging.warning(f"[GEMINI] {reason}. Using fallback assessment.")
        system_state.gemini_last_status = "not_configured"
        system_state.gemini_last_error = reason
        return generate_fallback_assessment(event, reason)

    logging.info(f"[GEMINI] Request started for device '{event.device_id}' (Impact: {event.impact_g:.2f}g)")

    prompt = f"""
    You are an intelligent event-analysis layer for an ESP32-C3 wearable fall detection system.
    A sensor-based fall event has been confirmed by hardware. Analyze the kinematic telemetry below:

    Telemetry Data:
    - Device ID: {event.device_id}
    - Timestamp: {event.timestamp}
    - Impact Force: {event.impact_g:.2f} g ({event.impact_g * 9.80665:.1f} m/s²)
    - Angular Velocity (Rotation): {event.rotation_rads:.2f} rad/s
    - Posture Orientation Shift: {event.posture_change_deg:.1f}°
    - Post-Impact Stillness Variation: {event.stillness_variation:.2f} m/s²
    - Hardware Fall Confidence: {event.fall_confidence * 100:.0f}%

    Instructions:
    1. Classify severity as 'LOW', 'MEDIUM', 'HIGH', or 'CRITICAL' based strictly on kinematic telemetry.
    2. Provide a 2-sentence concise kinematic analysis in 'assessment'.
    3. Provide a clear, empathetic, urgent message for caregivers in 'caregiver_message'.
    4. State the immediate recommended action in 'recommended_action'.

    STRICT SAFETY RULES:
    - DO NOT invent or fabricate sensor numbers not present in the input telemetry.
    - DO NOT state medical diagnoses (e.g., do NOT claim 'fractured bone', 'concussion', or 'internal bleeding'). Focus strictly on kinematic severity and patient immobility risk.
    - Return ONLY a valid JSON object matching the requested schema.
    """

    models_to_try = ["gemini-3-flash-preview", "gemini-3.8-flash", "gemini-flash-latest"]
    last_exception = None

    for model_name in models_to_try:
        try:
            client = genai.Client(api_key=api_key)
            response = client.models.generate_content(
                model=model_name,
                contents=prompt,
                config=types.GenerateContentConfig(
                    response_mime_type="application/json",
                    response_schema=GeminiAssessmentSchema,
                    temperature=0.2
                )
            )

            if response and response.text:
                parsed = json.loads(response.text)
                logging.info(f"[GEMINI] Response received successfully using model '{model_name}' (Severity: {parsed.get('severity')})")
                system_state.gemini_last_status = "working"
                system_state.gemini_last_error = None
                system_state.gemini_last_success_time = datetime.datetime.now(datetime.timezone.utc).isoformat()
                return parsed
            else:
                raise ValueError("Empty response text received from Gemini API")

        except Exception as e:
            last_exception = e
            logging.warning(f"[GEMINI] Model '{model_name}' request failed: {sanitize_secret(str(e), api_key)}")

    safe_error = parse_safe_gemini_error(last_exception)
    logging.error(f"[GEMINI] API request failed after model retries: {safe_error}")
    system_state.gemini_last_status = "error"
    system_state.gemini_last_error = safe_error
    return generate_fallback_assessment(event, safe_error)

# ============================================================
# TELEGRAM BOT HELPER
# ============================================================
def send_telegram_alert(
    event: FallEventRequest,
    assessment: Dict[str, Any],
    override_bot_token: Optional[str] = None,
    override_chat_id: Optional[str] = None
) -> tuple[bool, Optional[str]]:
    bot_token = override_bot_token if override_bot_token is not None else os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = override_chat_id if override_chat_id is not None else os.getenv("TELEGRAM_CHAT_ID")

    if not bot_token or not chat_id or bot_token.startswith("your_") or chat_id.startswith("your_"):
        reason = "Telegram delivery skipped: TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID unconfigured"
        logging.warning(f"[TELEGRAM] {reason}")
        system_state.telegram_last_status = "not_configured"
        system_state.telegram_last_error = reason
        return False, reason

    masked_chat = f"...{chat_id[-4:]}" if len(chat_id) >= 4 else "masked"
    logging.info(f"[TELEGRAM] Request started for chat ID '{masked_chat}'")

    impact_m_s2 = event.impact_g * 9.80665
    severity = str(assessment.get("severity", "HIGH")).upper()
    severity_emoji = "🔴" if severity in ["HIGH", "CRITICAL"] else "🟡"

    message = (
        f"🚨 POTENTIAL FALL ALERT DETECTED 🚨\n\n"
        f"Device ID: {event.device_id}\n"
        f"Timestamp: {event.timestamp}\n"
        f"Severity: {severity_emoji} {severity}\n\n"
        f"📊 Kinematic Telemetry:\n"
        f"• Impact Peak: {event.impact_g:.2f} g ({impact_m_s2:.1f} m/s²)\n"
        f"• Angular Rotation: {event.rotation_rads:.2f} rad/s\n"
        f"• Posture Shift: {event.posture_change_deg:.1f}°\n"
        f"• Stillness Variation: {event.stillness_variation:.2f} m/s²\n"
        f"• Hardware Confidence: {event.fall_confidence * 100:.0f}%\n\n"
        f"🧠 Gemini AI Assessment:\n"
        f"{assessment.get('assessment', 'No assessment available.')}\n\n"
        f"👉 Recommended Action:\n"
        f"{assessment.get('recommended_action', 'Check on wearer immediately.')}\n\n"
        f"💬 Caregiver Message:\n"
        f"{assessment.get('caregiver_message', 'Immediate attention recommended.')}"
    )

    url = f"https://api.telegram.org/bot{bot_token}/sendMessage"
    payload = {
        "chat_id": chat_id,
        "text": message
    }

    try:
        with httpx.Client(timeout=5.0) as client:
            resp = client.post(url, json=payload)
            if resp.status_code == 200:
                logging.info("[TELEGRAM] Delivery success (HTTP 200)")
                system_state.telegram_last_status = "delivered"
                system_state.telegram_last_error = None
                system_state.telegram_last_success_time = datetime.datetime.now(datetime.timezone.utc).isoformat()
                return True, None
            elif resp.status_code == 401:
                reason = "Telegram delivery failed: HTTP 401 Unauthorized"
                logging.error(f"[TELEGRAM] {reason}")
                system_state.telegram_last_status = "failed"
                system_state.telegram_last_error = reason
                return False, reason
            elif resp.status_code == 403:
                reason = "Telegram delivery failed: HTTP 403 Forbidden"
                logging.error(f"[TELEGRAM] {reason}")
                system_state.telegram_last_status = "failed"
                system_state.telegram_last_error = reason
                return False, reason
            else:
                raw_err = f"Telegram delivery failed: HTTP {resp.status_code}"
                reason = sanitize_secret(raw_err, bot_token)
                logging.error(f"[TELEGRAM] {reason}")
                system_state.telegram_last_status = "failed"
                system_state.telegram_last_error = reason
                return False, reason
    except httpx.TimeoutException:
        reason = "Telegram delivery failed: network timeout"
        logging.error(f"[TELEGRAM] {reason}")
        system_state.telegram_last_status = "failed"
        system_state.telegram_last_error = reason
        return False, reason
    except Exception as e:
        raw_err = f"Telegram delivery failed: {type(e).__name__} - {str(e)}"
        reason = sanitize_secret(raw_err, bot_token)
        logging.error(f"[TELEGRAM] {reason}")
        system_state.telegram_last_status = "failed"
        system_state.telegram_last_error = reason
        return False, reason

# ============================================================
# API ENDPOINTS
# ============================================================
@app.post("/api/device/heartbeat", status_code=status.HTTP_200_OK)
def receive_device_heartbeat(payload: HeartbeatPayload):
    system_state.update_device_heartbeat(
        device_id=payload.device_id,
        uptime_seconds=payload.uptime_seconds,
        wifi_rssi=payload.wifi_rssi
    )
    logging.info(f"[HEARTBEAT] Received heartbeat from device '{payload.device_id}' (RSSI: {payload.wifi_rssi}, Uptime: {payload.uptime_seconds}s)")
    return {
        "status": "ok",
        "message": "Heartbeat received",
        "device_id": payload.device_id
    }

@app.get("/api/device/status", response_model=DeviceStatusResponse)
def get_device_status(device_id: str = Query(default="ESP32C3_01", description="Device ID to check status for")):
    return DeviceStatusResponse(**system_state.get_device_status(device_id))

@app.get("/health")
def health_check():
    gemini_ok, gemini_cfg_msg = get_gemini_config_status()
    telegram_ok, tg_cfg_msg = get_telegram_config_status()

    if not gemini_ok:
        g_status = "not_configured"
        g_err = gemini_cfg_msg
    else:
        g_status = system_state.gemini_last_status
        g_err = system_state.gemini_last_error

    if not telegram_ok:
        t_status = "not_configured"
        t_err = tg_cfg_msg
    else:
        t_status = system_state.telegram_last_status
        t_err = system_state.telegram_last_error

    return {
        "status": "ok",
        "service": "fall-detection-backend",
        "gemini_api_configured": gemini_ok,
        "telegram_bot_configured": telegram_ok,
        "database_connected": True,
        "gemini_status": g_status,
        "gemini_last_error": g_err,
        "gemini_last_success_time": system_state.gemini_last_success_time,
        "telegram_status": t_status,
        "telegram_last_error": t_err,
        "telegram_last_success_time": system_state.telegram_last_success_time
    }

@app.post("/api/fall-event", response_model=FallEventResponse, status_code=status.HTTP_201_CREATED)
def process_fall_event(event: FallEventRequest):
    logging.info(f"Received fall event payload from device '{event.device_id}' (Impact: {event.impact_g:.2f}g)")

    db_event_id = None
    try:
        # 1. Store Raw Fall Event in SQLite first
        db_event_id = database.insert_raw_event(
            device_id=event.device_id,
            timestamp=event.timestamp,
            impact_g=event.impact_g,
            rotation_rads=event.rotation_rads,
            posture_change_deg=event.posture_change_deg,
            stillness_variation=event.stillness_variation,
            fall_confidence=event.fall_confidence
        )

        # 2. Run Gemini AI Assessment (or fallback)
        assessment = analyze_fall_with_gemini(event)

        # 3. Update Database with Gemini Result
        database.update_event_gemini(
            event_id=db_event_id,
            severity=assessment.get("severity", "UNKNOWN"),
            assessment=assessment.get("assessment", ""),
            caregiver_message=assessment.get("caregiver_message", ""),
            recommended_action=assessment.get("recommended_action", ""),
            alert_status="assessed"
        )

        # 4. Dispatch Telegram Alert
        telegram_delivered, tg_failure_reason = send_telegram_alert(event, assessment)

        # 5. Update Database with Telegram Status & Failure Reason
        if telegram_delivered:
            alert_status = "SENT"
            failure_reason = None
        else:
            alert_status = "FAILED"
            failure_reason = tg_failure_reason or "Telegram delivery failed"

        database.update_event_telegram(
            event_id=db_event_id,
            telegram_delivered=telegram_delivered,
            alert_status=alert_status,
            failure_reason=failure_reason
        )

        return FallEventResponse(
            event_id=str(db_event_id),
            status="processed",
            telegram_delivered=telegram_delivered,
            gemini_assessment=assessment
        )
    except Exception as e:
        safe_error = sanitize_secret(f"Alert processing failed: {type(e).__name__} - {str(e)}")
        logging.error(f"[PIPELINE ERROR] {safe_error}")
        if db_event_id:
            database.update_event_failure(
                event_id=db_event_id,
                alert_status="FAILED",
                failure_reason=safe_error
            )
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=safe_error
        )

@app.get("/api/events", response_model=EventHistoryResponse)
def get_event_history(
    limit: int = Query(default=50, ge=1, le=100, description="Maximum number of events to return (1-100)"),
    device_id: Optional[str] = Query(default=None, description="Optional filter by device ID")
):
    events = database.get_events(limit=limit, device_id=device_id)
    return EventHistoryResponse(events=events)

@app.get("/api/dashboard/latest")
def get_dashboard_latest():
    latest = database.get_latest_event()
    if not latest:
        return {"event": None}
    return latest

@app.get("/api/events/{event_id}")
def get_single_event(event_id: int):
    event_record = database.get_event_by_id(event_id)
    if not event_record:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Fall event with ID {event_id} not found"
        )
    return event_record

# Mount static files for browser dashboard
static_dir = os.path.join(os.path.dirname(__file__), "static")
if not os.path.exists(static_dir):
    os.makedirs(static_dir, exist_ok=True)
app.mount("/", StaticFiles(directory=static_dir, html=True), name="static")

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)


