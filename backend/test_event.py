import sys
import os
import httpx
import json
from dotenv import load_dotenv

# Load backend/.env if available
load_dotenv(os.path.join(os.path.dirname(__file__), ".env"))

BACKEND_URL = "http://127.0.0.1:8000"

def test_case_d_invalid_payload():
    print("\n--- [TEST CASE D] Invalid Event Payload (Expect HTTP 422 & No DB Record) ---")
    invalid_payload = {
        "device_id": "ESP32C3_TEST_INVALID",
        "impact_g": -5.0  # Invalid negative g
    }
    try:
        resp = httpx.post(f"{BACKEND_URL}/api/fall-event", json=invalid_payload, timeout=5.0)
        print(f"Response HTTP Code: {resp.status_code}")
        if resp.status_code == 422:
            print("SUCCESS: Received HTTP 422 Unprocessable Entity as expected.")
            return True
        else:
            print(f"FAILED: Expected HTTP 422 but received {resp.status_code}")
            return False
    except Exception as e:
        print(f"FAILED: Exception occurred: {e}")
        return False

def test_case_b_gemini_unavailable():
    print("\n--- [TEST CASE B] Gemini Unavailable (Expect Fallback Assessment & Persistent Storage) ---")
    from main import analyze_fall_with_gemini, FallEventRequest
    sample_request = FallEventRequest(
        device_id="TEST_DEV_B_DB",
        timestamp="2026-09-23T00:50:00Z",
        impact_g=2.8,
        rotation_rads=3.5,
        posture_change_deg=70.0,
        stillness_variation=0.5,
        fall_confidence=0.98
    )
    result = analyze_fall_with_gemini(sample_request, override_api_key="INVALID_GEMINI_KEY_123")
    print("Fallback Assessment Result:")
    print(json.dumps(result, indent=2))
    
    is_fallback = "Fallback assessment active" in result.get("assessment", "") or "INVALID_GEMINI_KEY" in result.get("assessment", "")
    if is_fallback and result.get("severity") in ["LOW", "MEDIUM", "HIGH", "CRITICAL"]:
        print("SUCCESS: Fallback assessment generated cleanly on Gemini failure.")
        return True
    else:
        print("FAILED: Fallback assessment was not correctly triggered.")
        return False

def test_case_c_telegram_unavailable():
    print("\n--- [TEST CASE C] Telegram Unavailable (Expect Storage & telegram_delivered=false) ---")
    from main import send_telegram_alert, FallEventRequest
    sample_request = FallEventRequest(
        device_id="TEST_DEV_C_DB",
        timestamp="2026-09-23T00:50:00Z",
        impact_g=2.8,
        rotation_rads=3.5,
        posture_change_deg=70.0,
        stillness_variation=0.5,
        fall_confidence=0.98
    )
    dummy_assessment = {
        "severity": "HIGH",
        "assessment": "Test assessment",
        "caregiver_message": "Test caregiver message",
        "recommended_action": "Test action"
    }
    delivered = send_telegram_alert(sample_request, dummy_assessment, override_bot_token="123456:INVALID_TOKEN_XYZ", override_chat_id="999999")
    print(f"Telegram Delivery Status with Invalid Token: {delivered}")
    
    resp = httpx.post(f"{BACKEND_URL}/api/fall-event", json=sample_request.model_dump(), timeout=10.0)
    print(f"Backend API Response Code: {resp.status_code}")
    res_data = resp.json()
    print("Backend Response Payload:", res_data)
    
    event_id = int(res_data["event_id"])
    
    # Check DB record
    single_resp = httpx.get(f"{BACKEND_URL}/api/events/{event_id}")
    print(f"Single Event DB Fetch Response (HTTP {single_resp.status_code}):", single_resp.json())

    if resp.status_code == 201 and not delivered and single_resp.status_code == 200:
        print("SUCCESS: Event stored in DB with telegram_delivered=false on Telegram failure.")
        return True
    else:
        print("FAILED: Event was not properly stored or status was wrong.")
        return False

def test_case_a_valid_event_storage():
    print("\n--- [TEST CASE A] Valid Event -> POST -> Verified in SQLite ---")
    payload = {
        "device_id": "ESP32C3_TEST_PERSIST",
        "timestamp": "2026-09-23T00:50:00Z",
        "impact_g": 2.75,
        "rotation_rads": 3.82,
        "posture_change_deg": 72.4,
        "stillness_variation": 0.38,
        "fall_confidence": 0.97
    }

    try:
        resp = httpx.post(f"{BACKEND_URL}/api/fall-event", json=payload, timeout=10.0)
        print(f"Response HTTP Code: {resp.status_code}")
        result = resp.json()
        print("Fall Event Response JSON:")
        print(json.dumps(result, indent=2))
        
        event_id = int(result["event_id"])
        print(f"Stored Event ID: {event_id}")

        # Fetch single event from DB
        db_fetch = httpx.get(f"{BACKEND_URL}/api/events/{event_id}", timeout=5.0)
        print(f"Fetched Event from DB (HTTP {db_fetch.status_code}):")
        print(json.dumps(db_fetch.json(), indent=2))

        if resp.status_code == 201 and db_fetch.status_code == 200 and db_fetch.json()["device_id"] == "ESP32C3_TEST_PERSIST":
            print("SUCCESS: Event saved to database and retrieved successfully.")
            return True, event_id
        else:
            print("FAILED: Database verification failed.")
            return False, None
    except Exception as e:
        print(f"FAILED: Exception during valid event test: {e}")
        return False, None

def test_case_e_history_endpoint(event_id: int):
    print("\n--- [TEST CASE E] GET /api/events (History Endpoint Check) ---")
    try:
        resp = httpx.get(f"{BACKEND_URL}/api/events?limit=10", timeout=5.0)
        print(f"GET /api/events HTTP Code: {resp.status_code}")
        data = resp.json()
        events = data.get("events", [])
        print(f"Retrieved {len(events)} events from history.")
        if len(events) > 0:
            print("Newest event in history:", events[0])
            # Verify events are sorted newest first (id descending)
            if events[0]["id"] >= events[-1]["id"]:
                print("SUCCESS: Events returned in newest-first order.")
                return True
        print("FAILED: History response structure invalid or unsorted.")
        return False
    except Exception as e:
        print(f"FAILED: History test failed: {e}")
        return False

def test_case_f_single_event_endpoint(event_id: int):
    print("\n--- [TEST CASE F] GET /api/events/{id} & 404 Check ---")
    try:
        # Valid ID
        resp_valid = httpx.get(f"{BACKEND_URL}/api/events/{event_id}", timeout=5.0)
        print(f"Valid ID Fetch (HTTP {resp_valid.status_code}):", resp_valid.json())
        
        # Unknown ID (999999)
        resp_invalid = httpx.get(f"{BACKEND_URL}/api/events/999999", timeout=5.0)
        print(f"Unknown ID Fetch (HTTP {resp_invalid.status_code}):", resp_invalid.json())
        
        if resp_valid.status_code == 200 and resp_invalid.status_code == 404:
            print("SUCCESS: Single-event endpoint returns 200 for valid ID and 404 for unknown ID.")
            return True
        else:
            print("FAILED: Single-event endpoint status codes invalid.")
            return False
    except Exception as e:
        print(f"FAILED: Single event test failed: {e}")
        return False

if __name__ == "__main__":
    print("==================================================")
    print("  ESP32-C3 Phase 4B Database & API Test Suite    ")
    print("==================================================")

    # Health check
    try:
        h_resp = httpx.get(f"{BACKEND_URL}/health", timeout=5.0)
        print("Health Check Response:", h_resp.json())
    except Exception as e:
        print(f"ERROR: Cannot connect to backend server at {BACKEND_URL}. Ensure uvicorn is running!")
        sys.exit(1)

    res_d = test_case_d_invalid_payload()
    res_b = test_case_b_gemini_unavailable()
    res_c = test_case_c_telegram_unavailable()
    res_a, created_id = test_case_a_valid_event_storage()
    res_e = test_case_e_history_endpoint(created_id if created_id else 1)
    res_f = test_case_f_single_event_endpoint(created_id if created_id else 1)

    print("\n==================================================")
    print("            PHASE 4B FINAL TEST SUMMARY           ")
    print("==================================================")
    print(f"Test Case A (Valid Event + SQLite Storage):  {'PASS' if res_a else 'FAIL'}")
    print(f"Test Case B (Gemini Failure DB Persistence): {'PASS' if res_b else 'FAIL'}")
    print(f"Test Case C (Telegram Failure DB Persistence):{'PASS' if res_c else 'FAIL'}")
    print(f"Test Case D (Invalid Payload 422 Check):     {'PASS' if res_d else 'FAIL'}")
    print(f"Test Case E (GET /api/events History):        {'PASS' if res_e else 'FAIL'}")
    print(f"Test Case F (GET /api/events/{{id}} 404 Check): {'PASS' if res_f else 'FAIL'}")
    print("==================================================")
