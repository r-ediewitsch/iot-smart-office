"""
database.py — SQLite helpers for Smart Office Dashboard
"""

import os
import sqlite3
from datetime import datetime

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'smart_office.db')

_last_state = {
    'headcount': None,
    'ac_target_temp': None,
    'ac_on_since': None,
    'last_uid': None,
    'last_action': None,
}

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    return conn

def init_db():
    with get_db() as conn:
        conn.executescript('''
            CREATE TABLE IF NOT EXISTS telemetry (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp       TEXT NOT NULL DEFAULT (datetime('now')),
                temperature     REAL NOT NULL,
                humidity        REAL NOT NULL,
                headcount       INTEGER NOT NULL DEFAULT 0,
                door_open       INTEGER NOT NULL DEFAULT 0,
                door_open_sec   INTEGER NOT NULL DEFAULT 0,
                ac_status       INTEGER NOT NULL DEFAULT 0,
                ac_target_temp  INTEGER NOT NULL DEFAULT 24,
                ac_health_alert INTEGER NOT NULL DEFAULT 0,
                last_uid        TEXT,
                last_action     TEXT
            );

            CREATE TABLE IF NOT EXISTS event_logs (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp   TEXT NOT NULL DEFAULT (datetime('now')),
                event_type  TEXT NOT NULL,
                message     TEXT NOT NULL,
                uid         TEXT
            );
        ''')

        # Migration: tambahkan kolom uid jika belum ada (DB lama)
        cols = [row[1] for row in conn.execute("PRAGMA table_info(event_logs)").fetchall()]
        if 'uid' not in cols:
            conn.execute("ALTER TABLE event_logs ADD COLUMN uid TEXT")

        # Migration: tambahkan kolom last_uid/last_action pada telemetry jika belum ada
        tel_cols = [row[1] for row in conn.execute("PRAGMA table_info(telemetry)").fetchall()]
        if 'last_uid' not in tel_cols:
            conn.execute("ALTER TABLE telemetry ADD COLUMN last_uid TEXT")
        if 'last_action' not in tel_cols:
            conn.execute("ALTER TABLE telemetry ADD COLUMN last_action TEXT")

def cleanup_old_telemetry():
    with get_db() as conn:
        conn.execute("DELETE FROM telemetry WHERE timestamp < datetime('now', '-1 day')")


def insert_telemetry(data: dict):
    cleanup_old_telemetry()
    with get_db() as conn:
        conn.execute('''
            INSERT INTO telemetry
                (temperature, humidity, headcount, door_open, door_open_sec,
                 ac_status, ac_target_temp, ac_health_alert, last_uid, last_action)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ''', (
            data['temp'],
            data['humidity'],
            data['headcount'],
            int(data['door_open']),
            data.get('door_open_sec', 0),
            int(data['ac_status']),
            data.get('ac_target_temp', 24),
            int(data.get('ac_health_alert', False)),
            str(data.get('last_uid')) if data.get('last_uid') else None,
            data.get('last_action')
        ))

def insert_event(event_type: str, message: str, uid: str = None):
    with get_db() as conn:
        conn.execute(
            'INSERT INTO event_logs (event_type, message, uid) VALUES (?, ?, ?)',
            (event_type, message, uid)
        )

def get_latest_telemetry():
    with get_db() as conn:
        row = conn.execute(
            'SELECT * FROM telemetry ORDER BY id DESC LIMIT 1'
        ).fetchone()
        if row:
            return dict(row)
    return None

def get_previous_telemetry():
    with get_db() as conn:
        rows = conn.execute(
            'SELECT * FROM telemetry ORDER BY id DESC LIMIT 2'
        ).fetchall()
        if len(rows) >= 2:
            return dict(rows[1])
    return None

def get_telemetry_history(minutes: int = 30, limit: int = 200):
    with get_db() as conn:
        rows = conn.execute('''
            SELECT * FROM telemetry
            WHERE timestamp >= datetime('now', ? || ' minutes')
            ORDER BY id ASC
            LIMIT ?
        ''', (f'-{minutes}', limit)).fetchall()
        return [dict(r) for r in rows]

def get_recent_events(limit: int = 50):
    with get_db() as conn:
        rows = conn.execute(
            'SELECT * FROM event_logs ORDER BY id DESC LIMIT ?',
            (limit,)
        ).fetchall()
        return [dict(r) for r in rows]

def get_telemetry_at_time(minutes_ago: int):
    with get_db() as conn:
        row = conn.execute('''
            SELECT * FROM telemetry
            WHERE timestamp <= datetime('now', ? || ' minutes')
            ORDER BY id DESC LIMIT 1
        ''', (f'-{minutes_ago}',)).fetchone()
        if row:
            return dict(row)
    return None

def detect_and_log_events(data: dict, is_demo: bool = False):
    prev = _last_state

    if prev['headcount'] is not None and prev['headcount'] > 0 and data['headcount'] == 0:
        insert_event('LAST_PERSON_OUT', 'Room empty — AC auto-OFF triggered')

    if prev['headcount'] is not None and prev['headcount'] == 0 and data['headcount'] > 0:
        insert_event('FIRST_PERSON_IN',
                     f"Staff entered — AC auto-ON at {data.get('ac_target_temp', 24)}°C")

    door_threshold = 10 if is_demo else 180
    if data['door_open'] and data.get('door_open_sec', 0) > door_threshold and data['ac_status']:
        insert_event('DOOR_LEAK',
                     f"Door open >{data['door_open_sec']}s while AC on — AC auto-OFF")

    if (prev['ac_target_temp'] is not None and
            data.get('ac_target_temp') != prev['ac_target_temp']):
        insert_event('CLIMATE_CHANGE',
                     f"Setpoint adjusted to {data['ac_target_temp']}°C")

    if data.get('ac_health_alert'):
        insert_event('HEALTH_ALERT',
                     'Compressor health warning — temp not dropping after 30 min')

    _last_state['headcount'] = data['headcount']
    _last_state['ac_target_temp'] = data.get('ac_target_temp')
