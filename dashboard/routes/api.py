"""
routes/api.py — REST API endpoints for Smart Office Dashboard
"""

import time
import random
import threading
from datetime import datetime
from flask import Blueprint, request, jsonify, current_app
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from database import (
    insert_telemetry, insert_event, get_latest_telemetry,
    get_telemetry_history, get_recent_events, detect_and_log_events
)
from mqtt_client import publish_command

api_bp = Blueprint('api', __name__)

demo_running = False
demo_thread = None


def demo_loop(app):
    temp = 24.0
    headcount = 0
    door_open = False
    ac_on = False
    target = 24

    with app.app_context():
        while demo_running:
            temp += random.uniform(-0.3, 0.3)
            temp = max(18.0, min(32.0, temp))

            if random.random() < 0.1:
                headcount = max(0, headcount + random.choice([-1, 1]))
            if random.random() < 0.05:
                door_open = not door_open

            ac_on = headcount > 0 and not (door_open and random.random() < 0.3)
            target = 24 if temp > 25.5 else (25 if temp < 21 else 24)

            payload = {
                "temp": round(temp, 1),
                "humidity": round(random.uniform(45, 70), 1),
                "headcount": headcount,
                "door_open": door_open,
                "door_open_sec": random.randint(0, 15) if door_open else 0,
                "ac_status": ac_on,
                "ac_target_temp": target,
                "ac_health_alert": False
            }

            insert_telemetry(payload)
            detect_and_log_events(payload, is_demo=True)

            time.sleep(2)


@api_bp.route('/telemetry', methods=['POST'])
def receive_telemetry():
    try:
        data = request.get_json(force=True)
        for field in ['temp', 'humidity', 'headcount', 'door_open', 'ac_status']:
            if field not in data:
                return jsonify({"error": f"Missing required field: {field}"}), 400

        insert_telemetry(data)
        detect_and_log_events(data)
        return jsonify({"status": "ok", "events_logged": 0}), 200
    except Exception as e:
        current_app.logger.error(f"Telemetry error: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route('/live-status', methods=['GET'])
def live_status():
    try:
        telemetry = get_latest_telemetry()
        if telemetry is None:
            return jsonify({}), 204
        return jsonify({
            "timestamp": telemetry['timestamp'],
            "temperature": telemetry['temperature'],
            "humidity": telemetry['humidity'],
            "headcount": telemetry['headcount'],
            "door_open": bool(telemetry['door_open']),
            "door_open_sec": telemetry['door_open_sec'],
            "ac_status": bool(telemetry['ac_status']),
            "ac_target_temp": telemetry['ac_target_temp'],
            "ac_health_alert": bool(telemetry['ac_health_alert'])
        }), 200
    except Exception as e:
        current_app.logger.error(f"Live status error: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route('/history', methods=['GET'])
def history():
    try:
        minutes = request.args.get('minutes', 30, type=int)
        limit = request.args.get('limit', 200, type=int)
        minutes = min(max(1, minutes), 1440)
        limit = min(max(1, limit), 500)

        data = get_telemetry_history(minutes, limit)
        return jsonify({"data": data, "count": len(data)}), 200
    except Exception as e:
        current_app.logger.error(f"History error: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route('/events', methods=['GET'])
def events():
    try:
        limit = request.args.get('limit', 15, type=int)
        limit = min(max(1, limit), 200)

        event_list = get_recent_events(limit)
        return jsonify({"events": event_list, "count": len(event_list)}), 200
    except Exception as e:
        current_app.logger.error(f"Events error: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route('/override', methods=['POST'])
def override():
    try:
        data = request.get_json(force=True)
        action = data.get('action', '')

        if action == 'turn_on':
            insert_event('MANUAL_OVERRIDE', 'Manual override: AC turned ON')
            publish_command({"action": "SET_POWER", "value": True})
            return jsonify({
                "status": "ok",
                "action": "turn_on",
                "message": "Manual override: AC turned ON"
            }), 200
        elif action == 'turn_off':
            insert_event('MANUAL_OVERRIDE', 'Manual override: AC turned OFF')
            publish_command({"action": "SET_POWER", "value": False})
            return jsonify({
                "status": "ok",
                "action": "turn_off",
                "message": "Manual override: AC turned OFF"
            }), 200
        elif action == 'set_temp':
            temp = data.get('temp', 24)
            insert_event('MANUAL_OVERRIDE', f'Manual override: Set temp to {temp}°C')
            publish_command({"action": "SET_TEMP", "target": temp})
            return jsonify({
                "status": "ok",
                "action": "set_temp",
                "message": f"Manual override: Set temp to {temp}°C"
            }), 200
        elif action == 'set_auto':
            insert_event('MANUAL_OVERRIDE', 'Manual override: Return to automatic mode')
            publish_command({"action": "SET_AUTO"})
            return jsonify({
                "status": "ok",
                "action": "set_auto",
                "message": "Return to automatic mode"
            }), 200
        else:
            return jsonify({"error": "Invalid action"}), 400
    except Exception as e:
        current_app.logger.error(f"Override error: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route('/demo', methods=['POST'])
def toggle_demo():
    global demo_running, demo_thread
    try:
        data = request.get_json(force=True)
        action = data.get('action', 'toggle')

        if action == 'start' and not demo_running:
            demo_running = True
            demo_thread = threading.Thread(
                target=demo_loop,
                args=(current_app._get_current_object(),),
                daemon=True
            )
            demo_thread.start()
            return jsonify({"demo": True, "message": "Demo mode started"}), 200
        elif action == 'stop':
            demo_running = False
            return jsonify({"demo": False, "message": "Demo mode stopped"}), 200
        elif action == 'toggle':
            if demo_running:
                demo_running = False
                return jsonify({"demo": False, "message": "Demo mode stopped"}), 200
            else:
                demo_running = True
                demo_thread = threading.Thread(
                    target=demo_loop,
                    args=(current_app._get_current_object(),),
                    daemon=True
                )
                demo_thread.start()
                return jsonify({"demo": True, "message": "Demo mode started"}), 200
        else:
            return jsonify({"error": "Invalid action"}), 400
    except Exception as e:
        current_app.logger.error(f"Demo toggle error: {e}")
        return jsonify({"error": str(e)}), 500
