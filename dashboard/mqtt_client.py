"""
mqtt_client.py — MQTT subscriber/publisher for Smart Office Dashboard
"""

import os
import json
import threading
import paho.mqtt.client as mqtt
from database import insert_telemetry, insert_event, detect_and_log_events

MQTT_BROKER = os.environ.get('MQTT_BROKER', 'localhost')
MQTT_PORT = int(os.environ.get('MQTT_PORT', 1883))

TOPIC_TELEMETRY = 'office/telemetry'
TOPIC_ACCESS = 'office/access/event'
TOPIC_COMMAND = 'office/ac/command'

_mqtt_client = None
_flask_app = None
demo_active = False


def is_demo_active():
    return demo_active


def set_demo_active(state: bool):
    global demo_active
    demo_active = state


def on_connect(client, userdata, flags, rc):
    client.subscribe(TOPIC_TELEMETRY)
    client.subscribe(TOPIC_ACCESS)
    print(f"[MQTT] Connected (rc={rc}), subscribed to topics")


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())

        with _flask_app.app_context():
            if msg.topic == TOPIC_TELEMETRY:
                insert_telemetry(payload)
                detect_and_log_events(payload)

            elif msg.topic == TOPIC_ACCESS:
                action = payload.get('action', 'UNKNOWN')
                uid = payload.get('uid', '?')
                headcount = payload.get('current_headcount', '?')
                label = 'MASUK' if action == 'IN' else 'KELUAR'
                message = f"Staf {label} (UID: {uid}) — headcount: {headcount}"
                insert_event('ACCESS', message, uid=str(uid))

    except Exception as e:
        print(f"[MQTT] Error processing message: {e}")


def publish_command(payload: dict):
    if _mqtt_client and _mqtt_client.is_connected():
        _mqtt_client.publish(TOPIC_COMMAND, json.dumps(payload))


def start_mqtt(app):
    global _mqtt_client, _flask_app
    _flask_app = app

    _mqtt_client = mqtt.Client()
    _mqtt_client.on_connect = on_connect
    _mqtt_client.on_message = on_message

    def _loop():
        try:
            _mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            _mqtt_client.loop_forever()
        except Exception as e:
            print(f"[MQTT] Connection failed: {e}")

    thread = threading.Thread(target=_loop, daemon=True)
    thread.start()
