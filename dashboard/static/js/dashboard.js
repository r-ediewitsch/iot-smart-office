const LIVE_POLL_MS = 2000;
const EVENT_POLL_MS = 5000;
const CHART_MAX_POINTS = 50;
const WIB_OFFSET = 7 * 60;

let chart = null;
let isDemoMode = false;

function parseUTCDate(isoString) {
    if (!isoString) return new Date();
    return new Date(isoString.replace(' ', 'T') + 'Z');
}

function formatWIBFull(isoString) {
    const d = parseUTCDate(isoString);
    const months = ['Jan', 'Feb', 'Mar', 'Apr', 'Mei', 'Jun', 'Jul', 'Agu', 'Sep', 'Okt', 'Nov', 'Des'];
    const day = d.getDate();
    const month = months[d.getMonth()];
    const year = d.getFullYear();
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    const ss = String(d.getSeconds()).padStart(2, '0');
    return `${day} ${month} ${year}, ${hh}:${mm}:${ss}`;
}

function formatWIBTime(isoString) {
    const d = parseUTCDate(isoString);
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    const ss = String(d.getSeconds()).padStart(2, '0');
    return `${hh}:${mm}:${ss}`;
}

async function fetchLiveStatus() {
    try {
        const res = await fetch('/api/live-status');
        if (res.status === 204) return;
        const data = await res.json();
        updateKPICards(data);
        updateChart(data);
        updateConnectionStatus(true);

        // Sync demo mode state with server
        if (data.demo_mode !== undefined && data.demo_mode !== isDemoMode) {
            isDemoMode = data.demo_mode;
            document.getElementById('btn-demo-toggle').classList.toggle('active', isDemoMode);
        }
    } catch (err) {
        updateConnectionStatus(false);
    }
}

async function fetchEventLog() {
    try {
        const res = await fetch('/api/events?limit=15');
        const { events } = await res.json();
        renderEventTable(events);
    } catch (err) {
        console.error('Event log fetch failed:', err);
    }
}

function updateConnectionStatus(connected) {
    const el = document.getElementById('connection-status');
    el.className = 'status-dot ' + (connected ? 'status-connected' : 'status-disconnected');
}

function updateKPICards(data) {
    const tempEl = document.getElementById('val-temperature');
    tempEl.textContent = `${data.temperature.toFixed(1)}\u00B0C`;
    tempEl.className = 'kpi-value ' + getTempStatusClass(data.temperature);

    document.getElementById('val-humidity').textContent = `${data.humidity.toFixed(1)}%`;

    document.getElementById('val-headcount').textContent = data.headcount;

    const doorEl = document.getElementById('val-door');
    doorEl.textContent = data.door_open ? 'Open' : 'Closed';
    doorEl.className = 'kpi-value ' + (data.door_open ? 'status-danger' : 'status-normal');

    const acEl = document.getElementById('val-ac');
    acEl.textContent = data.ac_status
        ? `ON ${data.ac_target_temp}\u00B0C`
        : 'OFF';
    acEl.className = 'kpi-value ' + (data.ac_status ? 'status-normal' : '');

    const healthEl = document.getElementById('ac-health-status');
    if (data.ac_health_alert) {
        healthEl.textContent = 'Alert';
        healthEl.className = 'status-danger';
    } else {
        healthEl.textContent = 'OK';
        healthEl.className = 'status-normal';
    }
}

function getTempStatusClass(temp) {
    if (temp < 21 || temp > 25.5) return 'status-danger';
    if (temp < 22 || temp > 25) return 'status-warning';
    return 'status-normal';
}

function initChart() {
    const ctx = document.getElementById('chart-temp-humidity').getContext('2d');
    chart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label: 'Temperature (\u00B0C)',
                    data: [],
                    borderColor: '#005fab',
                    backgroundColor: 'rgba(0, 95, 171, 0.08)',
                    fill: true,
                    tension: 0.4,
                    yAxisID: 'y-temp',
                },
                {
                    label: 'Humidity (%)',
                    data: [],
                    borderColor: '#0ea5e9',
                    backgroundColor: 'rgba(14, 165, 233, 0.08)',
                    fill: true,
                    tension: 0.4,
                    yAxisID: 'y-humid',
                },
                {
                    label: 'AC Target (\u00B0C)',
                    data: [],
                    borderColor: '#005fab',
                    borderDash: [5, 5],
                    pointRadius: 0,
                    fill: false,
                    tension: 0,
                    yAxisID: 'y-temp',
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: 300 },
            plugins: {
                legend: {
                    labels: { color: '#64748b', font: { family: 'Inter', size: 12 } }
                }
            },
            scales: {
                x: {
                    ticks: { color: '#94a3b8', maxTicksLimit: 10 },
                    grid: { color: '#f1f5f9' }
                },
                'y-temp': {
                    position: 'left',
                    min: 16, max: 35,
                    ticks: { color: '#005fab' },
                    grid: { color: '#f1f5f9' },
                    title: { display: true, text: '\u00B0C', color: '#005fab' }
                },
                'y-humid': {
                    position: 'right',
                    min: 20, max: 100,
                    ticks: { color: '#0ea5e9' },
                    grid: { drawOnChartArea: false },
                    title: { display: true, text: '%', color: '#0ea5e9' }
                }
            }
        }
    });
}

function updateChart(data) {
    if (!chart) return;

    const timeLabel = formatWIBTime(data.timestamp);

    chart.data.labels.push(timeLabel);
    chart.data.datasets[0].data.push(data.temperature);
    chart.data.datasets[1].data.push(data.humidity);
    chart.data.datasets[2].data.push(data.ac_target_temp);

    if (chart.data.labels.length > CHART_MAX_POINTS) {
        chart.data.labels.shift();
        chart.data.datasets.forEach(ds => ds.data.shift());
    }

    chart.update('none');
}

async function loadChartHistory() {
    initChart();
    try {
        const res = await fetch('/api/history?minutes=30&limit=50');
        const { data } = await res.json();
        data.forEach(row => updateChart(row));
    } catch (err) {
        console.error('Failed to load chart history:', err);
    }
}

function renderEventTable(events) {
    const tbody = document.getElementById('event-log-body');
    tbody.innerHTML = events.map(e => `
        <tr class="event-row event-${e.event_type}">
            <td>${formatWIBFull(e.timestamp)}</td>
            <td><span class="event-badge">${e.event_type}</span></td>
            <td class="uid-cell">${e.uid || '-'}</td>
            <td>${e.message}</td>
        </tr>
    `).join('');
}

lucide.createIcons();

document.getElementById('btn-demo-toggle').addEventListener('click', async () => {
    isDemoMode = !isDemoMode;
    const res = await fetch('/api/demo', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: isDemoMode ? 'start' : 'stop' })
    });
    const result = await res.json();
    isDemoMode = result.demo;
    document.getElementById('btn-demo-toggle').classList.toggle('active', isDemoMode);
});

document.getElementById('btn-ac-on').addEventListener('click', async () => {
    await fetch('/api/override', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'turn_on' })
    });
    fetchEventLog();
});

document.getElementById('btn-ac-off').addEventListener('click', async () => {
    await fetch('/api/override', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'turn_off' })
    });
    fetchEventLog();
});

document.getElementById('btn-set-temp').addEventListener('click', async () => {
    const temp = parseInt(document.getElementById('set-temp').value);
    if (temp >= 18 && temp <= 30) {
        await fetch('/api/override', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'set_temp', temp: temp })
        });
        fetchEventLog();
    }
});

document.getElementById('btn-set-auto').addEventListener('click', async () => {
    await fetch('/api/override', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'set_auto' })
    });
    fetchEventLog();
});

setInterval(fetchLiveStatus, LIVE_POLL_MS);
setInterval(fetchEventLog, EVENT_POLL_MS);

fetchLiveStatus();
fetchEventLog();
loadChartHistory();
