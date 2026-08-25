"""
Smart Office Microclimate — Flask Dashboard Server
Entry point: initializes database, registers blueprints, starts the app.
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from flask import Flask
from database import init_db
from routes.api import api_bp
from routes.views import views_bp
from mqtt_client import start_mqtt

app = Flask(__name__)

app.register_blueprint(api_bp, url_prefix='/api')
app.register_blueprint(views_bp)


@app.after_request
def add_cors_headers(response):
    response.headers['Access-Control-Allow-Origin'] = '*'
    response.headers['Access-Control-Allow-Headers'] = 'Content-Type'
    response.headers['Access-Control-Allow-Methods'] = 'GET, POST, OPTIONS'
    return response


if __name__ == '__main__':
    init_db()
    start_mqtt(app)
    app.run(host='0.0.0.0', port=5000, debug=True)
