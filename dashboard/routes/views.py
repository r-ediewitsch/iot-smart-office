"""
routes/views.py — HTML-serving routes for Smart Office Dashboard
"""

from flask import Blueprint, render_template

views_bp = Blueprint('views', __name__)


@views_bp.route('/')
def dashboard():
    return render_template('dashboard.html')
