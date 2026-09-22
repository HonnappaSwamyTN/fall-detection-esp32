import os
import json
import logging
from typing import Dict, Any, Optional, List
from fastapi import FastAPI, HTTPException, status, Query
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
# PYDANTIC SCHEMAS
# ============================================================
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
        "assessment": f"Sensor-confirmed fall event (Impact: {event.impact_g:.2f}g, Posture Tilt: {event.posture_change_deg:.1f}°). Fallback assessment active ({reason}).",
        "caregiver_message": "URGENT ALERT: A potential fall was detected by the wearable device. Please check on the wearer immediately!",
        "recommended_action": "Attempt to contact the wearer or visit their location immediately to verify safety."
    }

def analyze_fall_with_gemini(event: FallEventRequest, override_api_key: Optional[str] = None) -> Dict[str, Any]:
    api_key = override_api_key if override_api_key is not None else os.getenv("GEMINI_API_KEY")
    
    if not api_key or api_key.startswith("your_"):
        logging.warning("[GEMINI] GEMINI_API_KEY is missing or unconfigured. Using fallback assessment.")
        return generate_fallback_assessment(event, "Missing or unconfigured GEMINI_API_KEY")

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

    models_to_try = ["gemini-2.5-flash", "gemini-1.5-flash"]
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
                return parsed
            else:
                raise ValueError("Empty response text received from Gemini API")

        except Exception as e:
            last_exception = e
            logging.warning(f"[GEMINI] Model '{model_name}' request failed: {e}")

    logging.error(f"[GEMINI] API request failed after model retries: {last_exception}")
    return generate_fallback_assessment(event, f"Gemini API error: {str(last_exception)}")

# ============================================================
# TELEGRAM BOT HELPER
# ============================================================
def send_telegram_alert(event: FallEventRequest, assessment: Dict[str, Any], override_bot_token: Optional[str] = None, override_chat_id: Optional[str] = None) -> bool:
    bot_token = override_bot_token if override_bot_token is not None else os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = override_chat_id if override_chat_id is not None else os.getenv("TELEGRAM_CHAT_ID")

    if not bot_token or not chat_id or bot_token.startswith("your_") or chat_id.startswith("your_"):
        logging.warning("[TELEGRAM] TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID unconfigured. Skipping Telegram dispatch.")
        return False

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
        with httpx.Client(timeout=10.0) as client:
            resp = client.post(url, json=payload)
            if resp.status_code == 200:
                logging.info("[TELEGRAM] Delivery success (HTTP 200)")
                return True
            else:
                logging.error(f"[TELEGRAM] Delivery failure: HTTP {resp.status_code} - {resp.text}")
                return False
    except Exception as e:
        logging.error(f"[TELEGRAM] Delivery error: {e}")
        return False

# ============================================================
# API ENDPOINTS
# ============================================================
@app.get("/health")
def health_check():
    gemini_key = os.getenv("GEMINI_API_KEY", "")
    bot_token  = os.getenv("TELEGRAM_BOT_TOKEN", "")
    chat_id    = os.getenv("TELEGRAM_CHAT_ID", "")

    return {
        "status": "ok",
        "service": "fall-detection-backend",
        "gemini_api_configured": bool(gemini_key and not gemini_key.startswith("your_")),
        "telegram_bot_configured": bool(bot_token and not bot_token.startswith("your_") and chat_id and not chat_id.startswith("your_"))
    }

@app.post("/api/fall-event", response_model=FallEventResponse, status_code=status.HTTP_201_CREATED)
def process_fall_event(event: FallEventRequest):
    logging.info(f"Received fall event payload from device '{event.device_id}' (Impact: {event.impact_g:.2f}g)")

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
    telegram_delivered = send_telegram_alert(event, assessment)

    # 5. Update Database with Telegram Status
    alert_status = "alert_sent" if telegram_delivered else "alert_failed_or_skipped"
    database.update_event_telegram(
        event_id=db_event_id,
        telegram_delivered=telegram_delivered,
        alert_status=alert_status
    )

    return FallEventResponse(
        event_id=str(db_event_id),
        status="processed",
        telegram_delivered=telegram_delivered,
        gemini_assessment=assessment
    )

@app.get("/api/events", response_model=EventHistoryResponse)
def get_event_history(
    limit: int = Query(default=50, ge=1, le=100, description="Maximum number of events to return (1-100)"),
    device_id: Optional[str] = Query(default=None, description="Optional filter by device ID")
):
    events = database.get_events(limit=limit, device_id=device_id)
    return EventHistoryResponse(events=events)

@app.get("/api/events/{event_id}")
def get_single_event(event_id: int):
    event_record = database.get_event_by_id(event_id)
    if not event_record:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Fall event with ID {event_id} not found"
        )
    return event_record

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
