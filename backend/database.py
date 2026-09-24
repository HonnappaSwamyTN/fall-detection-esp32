import os
import sqlite3
import datetime
import logging
from typing import List, Dict, Any, Optional

DB_PATH = os.path.join(os.path.dirname(__file__), "events.db")

def get_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    logging.info(f"[DB] Initializing SQLite database at '{DB_PATH}'")
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("""
            CREATE TABLE IF NOT EXISTS fall_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                timestamp TEXT NOT NULL,
                impact_g REAL,
                rotation_rads REAL,
                posture_change_deg REAL,
                stillness_variation REAL,
                fall_confidence REAL,
                severity TEXT,
                assessment TEXT,
                caregiver_message TEXT,
                recommended_action TEXT,
                telegram_delivered INTEGER DEFAULT 0,
                alert_status TEXT,
                failure_reason TEXT,
                created_at TEXT NOT NULL
            );
            """)
            cursor.execute("CREATE INDEX IF NOT EXISTS idx_fall_events_device_id ON fall_events(device_id);")
            cursor.execute("CREATE INDEX IF NOT EXISTS idx_fall_events_timestamp ON fall_events(timestamp);")

            # Migration: ensure failure_reason column exists in existing SQLite tables
            cursor.execute("PRAGMA table_info(fall_events);")
            columns = [row[1] for row in cursor.fetchall()]
            if "failure_reason" not in columns:
                logging.info("[DB] Adding missing 'failure_reason' column to fall_events table.")
                cursor.execute("ALTER TABLE fall_events ADD COLUMN failure_reason TEXT;")

            conn.commit()
            logging.info("[DB] SQLite database initialized successfully.")
    except Exception as e:
        logging.error(f"[DB] Database initialization error: {e}")
        raise e

def insert_raw_event(
    device_id: str,
    timestamp: str,
    impact_g: float,
    rotation_rads: float,
    posture_change_deg: float,
    stillness_variation: float,
    fall_confidence: float
) -> int:
    created_at = datetime.datetime.now(datetime.timezone.utc).isoformat()
    alert_status = "received"
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("""
            INSERT INTO fall_events (
                device_id, timestamp, impact_g, rotation_rads,
                posture_change_deg, stillness_variation, fall_confidence,
                alert_status, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                device_id, timestamp, impact_g, rotation_rads,
                posture_change_deg, stillness_variation, fall_confidence,
                alert_status, created_at
            ))
            conn.commit()
            event_id = cursor.lastrowid
            logging.info(f"[DB] Raw event inserted with ID: {event_id}")
            return event_id
    except Exception as e:
        logging.error(f"[DB] Failed to insert raw event: {e}")
        raise e

def update_event_gemini(
    event_id: int,
    severity: str,
    assessment: str,
    caregiver_message: str,
    recommended_action: str,
    alert_status: str = "assessed"
):
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("""
            UPDATE fall_events SET
                severity = ?,
                assessment = ?,
                caregiver_message = ?,
                recommended_action = ?,
                alert_status = ?
            WHERE id = ?
            """, (
                severity, assessment, caregiver_message, recommended_action,
                alert_status, event_id
            ))
            conn.commit()
            logging.info(f"[DB] Event {event_id} updated with Gemini assessment.")
    except Exception as e:
        logging.error(f"[DB] Failed to update event {event_id} with Gemini assessment: {e}")

def update_event_telegram(
    event_id: int,
    telegram_delivered: bool,
    alert_status: str,
    failure_reason: Optional[str] = None
):
    delivered_int = 1 if telegram_delivered else 0
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("""
            UPDATE fall_events SET
                telegram_delivered = ?,
                alert_status = ?,
                failure_reason = ?
            WHERE id = ?
            """, (delivered_int, alert_status, failure_reason, event_id))
            conn.commit()
            logging.info(f"[DB] Event {event_id} updated with Telegram status (Delivered: {telegram_delivered}, Reason: {failure_reason}).")
    except Exception as e:
        logging.error(f"[DB] Failed to update event {event_id} with Telegram status: {e}")

def update_event_failure(
    event_id: int,
    alert_status: str,
    failure_reason: str
):
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("""
            UPDATE fall_events SET
                alert_status = ?,
                failure_reason = ?
            WHERE id = ?
            """, (alert_status, failure_reason, event_id))
            conn.commit()
            logging.info(f"[DB] Event {event_id} updated with failure status: {failure_reason}")
    except Exception as e:
        logging.error(f"[DB] Failed to update event {event_id} failure status: {e}")

def get_events(limit: int = 50, device_id: Optional[str] = None) -> List[Dict[str, Any]]:
    # Cap maximum limit at 100
    safe_limit = min(max(1, limit), 100)
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            if device_id:
                cursor.execute("""
                SELECT * FROM fall_events
                WHERE device_id = ?
                ORDER BY id DESC
                LIMIT ?
                """, (device_id, safe_limit))
            else:
                cursor.execute("""
                SELECT * FROM fall_events
                ORDER BY id DESC
                LIMIT ?
                """, (safe_limit,))
            
            rows = cursor.fetchall()
            events = []
            for row in rows:
                event_dict = dict(row)
                event_dict["telegram_delivered"] = bool(event_dict.get("telegram_delivered", 0))
                event_dict["event_id"] = event_dict["id"]
                if "failure_reason" not in event_dict or event_dict["failure_reason"] is None:
                    event_dict["failure_reason"] = None
                events.append(event_dict)
            return events
    except Exception as e:
        logging.error(f"[DB] Error fetching events history: {e}")
        return []

def get_event_by_id(event_id: int) -> Optional[Dict[str, Any]]:
    try:
        with get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("SELECT * FROM fall_events WHERE id = ?", (event_id,))
            row = cursor.fetchone()
            if row:
                event_dict = dict(row)
                event_dict["telegram_delivered"] = bool(event_dict.get("telegram_delivered", 0))
                event_dict["event_id"] = event_dict["id"]
                if "failure_reason" not in event_dict or event_dict["failure_reason"] is None:
                    event_dict["failure_reason"] = None
                return event_dict
            return None
    except Exception as e:
        logging.error(f"[DB] Error fetching event by ID {event_id}: {e}")
        return None

def get_latest_event(device_id: Optional[str] = None) -> Optional[Dict[str, Any]]:
    events = get_events(limit=1, device_id=device_id)
    if events:
        event = events[0]
        event["event_id"] = event["id"]
        return event
    return None


