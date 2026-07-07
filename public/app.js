const activeConnEl = document.getElementById('active-conn');
const totalReqEl = document.getElementById('total-req');
const reqSecEl = document.getElementById('req-sec');
const statusEl = document.getElementById('attack-status');

// Setup Chart.js
Chart.defaults.color = '#94a3b8';
Chart.defaults.borderColor = 'rgba(255, 255, 255, 0.1)';

const ctx = document.getElementById('trafficChart').getContext('2d');
const trafficChart = new Chart(ctx, {
    type: 'line',
    data: {
        labels: Array(20).fill(''),
        datasets: [{
            label: 'Requests / Sec',
            data: Array(20).fill(0),
            borderColor: '#a855f7',
            backgroundColor: 'rgba(168, 85, 247, 0.1)',
            borderWidth: 2,
            tension: 0.4,
            fill: true
        }, {
            label: 'Active Connections',
            data: Array(20).fill(0),
            borderColor: '#3b82f6',
            backgroundColor: 'rgba(59, 130, 246, 0.1)',
            borderWidth: 2,
            tension: 0.4,
            fill: true
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: {
            duration: 0
        },
        scales: {
            y: {
                beginAtZero: true
            }
        },
        plugins: {
            legend: {
                position: 'top',
            }
        }
    }
});

let lastTotalReq = 0;

// Connect to Server-Sent Events (SSE)
const evtSource = new EventSource('/api/metrics/stream');

evtSource.onmessage = function(event) {
    const data = JSON.parse(event.data);
    
    // Update raw numbers
    activeConnEl.innerText = data.active;
    totalReqEl.innerText = data.requests;
    
    // Calculate requests per second
    const rps = Math.max(0, data.requests - lastTotalReq);
    reqSecEl.innerText = rps;
    lastTotalReq = data.requests;

    // Update Chart
    trafficChart.data.datasets[0].data.shift();
    trafficChart.data.datasets[0].data.push(rps);
    
    trafficChart.data.datasets[1].data.shift();
    trafficChart.data.datasets[1].data.push(data.active);
    
    trafficChart.update();
};

evtSource.onerror = function() {
    console.error("SSE connection lost. Reconnecting...");
};

function showStatus(msg, color) {
    statusEl.innerText = msg;
    statusEl.className = `mt-4 text-sm font-medium text-center text-${color}-400 block`;
    setTimeout(() => { statusEl.classList.add('hidden'); }, 3000);
}

function triggerFlood() {
    fetch('/api/chaos/flood', { method: 'POST' })
        .then(() => showStatus('Flood launched! Watch the RPS spike.', 'blue'))
        .catch(() => showStatus('Failed to launch flood.', 'red'));
}

function triggerSlowloris() {
    fetch('/api/chaos/slowloris', { method: 'POST' })
        .then(() => showStatus('Slowloris launched! Connections will spike, then get killed by timeouts.', 'orange'))
        .catch(() => showStatus('Failed to launch Slowloris.', 'red'));
}
