import sys
import os
import httpx
import json
from dotenv import load_dotenv

# Load backend/.env if available
load_dotenv(os.path.join(os.path.dirname(__file__), ".env"))

BACKEND_URL = "http://127.0.0.1:8000"

def test_case_d_invalid_payload():
    print("\n--- [TEST CASE D] Invalid Event Payload (Expect HTTP 422) ---")
    invalid_payload = {
        "device_id": "ESP32C3_TEST",
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
    print("\n--- [TEST CASE B] Gemini Unavailable (Expect Fallback Assessment) ---")
    from main import analyze_fall_with_gemini, FallEventRequest
    sample_request = FallEventRequest(
        device_id="TEST_DEV_B",
        timestamp="2026-09-23T00:30:00Z",
        impact_g=2.8,
        rotation_rads=3.5,
        posture_change_deg=70.0,
        stillness_variation=0.5,
        fall_confidence=0.98
    )
    # Test with invalid key to force Gemini failure / fallback
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
    print("\n--- [TEST CASE C] Telegram Unavailable (Expect Valid Processed Response) ---")
    from main import send_telegram_alert, FallEventRequest
    sample_request = FallEventRequest(
        device_id="TEST_DEV_C",
        timestamp="2026-09-23T00:30:00Z",
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
    # Test with invalid token to force Telegram failure
    delivered = send_telegram_alert(sample_request, dummy_assessment, override_bot_token="123456:INVALID_TOKEN_XYZ", override_chat_id="999999")
    print(f"Telegram Delivery Status with Invalid Token: {delivered}")
    
    # Send request to backend endpoint to ensure backend returns HTTP 201 even when Telegram delivery fails
    resp = httpx.post(f"{BACKEND_URL}/api/fall-event", json=sample_request.model_dump(), timeout=10.0)
    print(f"Backend API Response Code: {resp.status_code}")
    print("Backend Response Payload:", resp.json())
    
    if resp.status_code == 201 and not delivered:
        print("SUCCESS: Backend handles Telegram delivery failure gracefully and returns HTTP 201.")
        return True
    else:
        print("FAILED: Backend response was not 201 or handling failed.")
        return False

def test_case_a_full_pipeline():
    print("\n--- [TEST CASE A] Real Gemini + Real Telegram Integration Pipeline ---")
    payload = {
        "device_id": "ESP32C3_TEST_REAL",
        "timestamp": "2026-09-23T00:35:00Z",
        "impact_g": 2.75,
        "rotation_rads": 3.82,
        "posture_change_deg": 72.4,
        "stillness_variation": 0.38,
        "fall_confidence": 0.97
    }

    try:
        resp = httpx.post(f"{BACKEND_URL}/api/fall-event", json=payload, timeout=20.0)
        print(f"Response HTTP Code: {resp.status_code}")
        result = resp.json()
        print("Full Event Response JSON:")
        print(json.dumps(result, indent=2))
        
        gemini_active = "Fallback assessment active" not in result.get("gemini_assessment", {}).get("assessment", "")
        telegram_active = result.get("telegram_delivered", False)
        
        print(f"-> Real Gemini Assessment Succeeded: {gemini_active}")
        print(f"-> Real Telegram Message Delivered: {telegram_active}")

        if resp.status_code == 201:
            print("SUCCESS: Full pipeline test executed.")
            return True, gemini_active, telegram_active
        else:
            print(f"FAILED: Response code was {resp.status_code}")
            return False, False, False
    except Exception as e:
        print(f"FAILED: Exception during full pipeline test: {e}")
        return False, False, False

if __name__ == "__main__":
    print("==================================================")
    print("  ESP32-C3 Fall Detection AI Backend Test Suite  ")
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
    res_a, real_gemini, real_telegram = test_case_a_full_pipeline()

    print("\n==================================================")
    print("               FINAL TEST SUMMARY                 ")
    print("==================================================")
    print(f"Test Case A (Full Pipeline Execution): {'PASS' if res_a else 'FAIL'}")
    print(f"  - Real Gemini API Call Succeeded: {real_gemini}")
    print(f"  - Real Telegram Alert Delivered:  {real_telegram}")
    print(f"Test Case B (Gemini Fallback Handling): {'PASS' if res_b else 'FAIL'}")
    print(f"Test Case C (Telegram Failure Handling): {'PASS' if res_c else 'FAIL'}")
    print(f"Test Case D (Invalid Payload 422 Check): {'PASS' if res_d else 'FAIL'}")
    print("==================================================")
