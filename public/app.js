const activeConnEl = document.getElementById('active-conn');
const rpsCountEl = document.getElementById('rps-count');
const blockedCountEl = document.getElementById('blocked-count');
const queueDepthEl = document.getElementById('queue-depth');
const queueBarEl = document.getElementById('queue-bar');
const logTailEl = document.getElementById('log-tail');
const logFallbackEl = document.getElementById('log-fallback');
const chartsFallbackEl = document.getElementById('charts-fallback');
const serverBadgeEl = document.getElementById('server-badge');
const serverTextEl = document.getElementById('server-text');
const serverDotEl = document.getElementById('server-dot');

const tAcceptEl = document.getElementById('t-accept');
const tParseEl = document.getElementById('t-parse');
const tRouteEl = document.getElementById('t-route');
const tcpEstEl = document.getElementById('tcp-est');
const tcpTwEl = document.getElementById('tcp-tw');
const tcpKaEl = document.getElementById('tcp-ka');
const tcpNewEl = document.getElementById('tcp-new');

let rpsHistory = new Array(20).fill(0);
let fdHistory = new Array(20).fill(0);

let methodCounts = { GET: 0, POST: 0, OTHER: 0 };
let latencyBuckets = [0, 0, 0, 0, 0]; // <1ms, 1-5ms, 5-10ms, 10-50ms, >50ms

let previousRequests = 0;
let hasData = false;
let proxyReqCache = {};

Chart.defaults.color = '#94a3b8';
Chart.defaults.font.family = 'monospace';

const trafficChartCtx = document.getElementById('trafficChart').getContext('2d');
const trafficChart = new Chart(trafficChartCtx, {
    type: 'line',
    data: {
        labels: new Array(20).fill(''),
        datasets: [
            {
                label: 'Requests/sec',
                data: rpsHistory,
                borderColor: '#34d399',
                backgroundColor: 'rgba(52, 211, 153, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.4
            },
            {
                label: 'Active FDs',
                data: fdHistory,
                borderColor: '#60a5fa',
                backgroundColor: 'rgba(96, 165, 250, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.4,
                yAxisID: 'y1'
            }
        ]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: { duration: 0 },
        scales: {
            x: { display: false },
            y: { beginAtZero: true, grid: { color: 'rgba(255,255,255,0.05)' } },
            y1: { beginAtZero: true, position: 'right', grid: { display: false } }
        },
        plugins: { legend: { position: 'top', labels: { boxWidth: 12 } } }
    }
});

const methodChartCtx = document.getElementById('methodChart').getContext('2d');
const methodChart = new Chart(methodChartCtx, {
    type: 'doughnut',
    data: {
        labels: ['GET', 'POST', 'OTHER'],
        datasets: [{
            data: [0, 0, 0],
            backgroundColor: ['#3b82f6', '#10b981', '#64748b'],
            borderWidth: 0
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        plugins: { legend: { position: 'right', labels: { boxWidth: 10, font: { size: 10 } } } },
        cutout: '70%'
    }
});

const latencyChartCtx = document.getElementById('latencyChart').getContext('2d');
const latencyChart = new Chart(latencyChartCtx, {
    type: 'bar',
    data: {
        labels: ['<1', '1-5', '5-10', '10-50', '>50'],
        datasets: [{
            data: [0, 0, 0, 0, 0],
            backgroundColor: '#8b5cf6',
            borderRadius: 4
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        plugins: { legend: { display: false } },
        scales: {
            x: { grid: { display: false }, ticks: { font: { size: 10 } } },
            y: { display: false }
        }
    }
});

const evtSource = new EventSource("/api/metrics/stream");

evtSource.onmessage = function(event) {
    if (!hasData) {
        hasData = true;
        previousRequests = JSON.parse(event.data).requests;
        chartsFallbackEl.classList.add('opacity-0');
        setTimeout(() => chartsFallbackEl.classList.add('hidden'), 300);
    }
    
    serverBadgeEl.className = "flex items-center space-x-3 bg-green-500/10 px-4 py-2 rounded-full border border-green-500/20 transition-colors";
    serverTextEl.innerText = "Server Online";
    serverDotEl.className = "w-3 h-3 bg-green-500 rounded-full animate-pulse";
    
    const data = JSON.parse(event.data);
    activeConnEl.innerText = data.active;
    blockedCountEl.innerText = data.blocked;
    
    const qDepth = data.queue;
    queueDepthEl.innerText = qDepth;
    const maxQueue = 100; 
    let percent = (qDepth / maxQueue) * 100;
    if (percent > 100) percent = 100;
    queueBarEl.style.width = percent + '%';
    
    if (qDepth > 50) {
        queueBarEl.className = "bg-red-500 h-2.5 rounded-full transition-all duration-300";
    } else if (qDepth > 10) {
        queueBarEl.className = "bg-orange-500 h-2.5 rounded-full transition-all duration-300";
    } else {
        queueBarEl.className = "bg-purple-500 h-2.5 rounded-full transition-all duration-300";
    }

    const currentRequests = data.requests;
    const rps = currentRequests - previousRequests;
    previousRequests = currentRequests;
    rpsCountEl.innerText = rps;

    rpsHistory.shift(); rpsHistory.push(rps);
    fdHistory.shift(); fdHistory.push(data.active);
    trafficChart.update();
    
    tcpEstEl.innerText = "0";
    tcpTwEl.innerText = "0";
    if (data.tcp_states) {
        if (data.tcp_states.ESTABLISHED !== undefined) tcpEstEl.innerText = data.tcp_states.ESTABLISHED;
        if (data.tcp_states.TIME_WAIT !== undefined) tcpTwEl.innerText = data.tcp_states.TIME_WAIT;
    }
    tcpKaEl.innerText = data.keep_alive_hits || 0;
    tcpNewEl.innerText = data.new_connections || 0;

    let avgAccept = 0, avgParse = 0, avgRoute = 0;
    let timingCount = 0;

    if (data.logs && data.logs.length > 0) {
        if (logFallbackEl) logFallbackEl.remove();
        
        data.logs.forEach(log => {
            try {
                const div = document.createElement('div');
                div.className = "flex space-x-3 opacity-0 transition-opacity duration-300";
                
                let colorClass = "text-green-400";
                if (log.status >= 400 && log.status < 500) colorClass = "text-yellow-400";
                if (log.status >= 500) colorClass = "text-red-400";
                
                const time = new Date().toLocaleTimeString('en-US', { hour12: false, hour: "numeric", minute: "numeric", second: "numeric", fractionalSecondDigits: 3 });
                
                div.innerHTML = `
                    <span class="text-gray-500">[${time}]</span>
                    <span class="text-blue-300 font-bold w-12">${log.method}</span>
                    <span class="text-gray-300 flex-1 truncate">${log.path}</span>
                    <span class="${colorClass} font-bold">${log.status}</span>
                    <span class="text-gray-500 w-16 text-right">${(log.latency_us / 1000).toFixed(1)}ms</span>
                `;
                logTailEl.appendChild(div);
                
                requestAnimationFrame(() => div.classList.remove('opacity-0'));
                
                if (log.method === 'GET') methodCounts.GET++;
                else if (log.method === 'POST') methodCounts.POST++;
                else methodCounts.OTHER++;
                
                const ms = log.latency_us / 1000;
                if (ms < 1) latencyBuckets[0]++;
                else if (ms < 5) latencyBuckets[1]++;
                else if (ms < 10) latencyBuckets[2]++;
                else if (ms < 50) latencyBuckets[3]++;
                else latencyBuckets[4]++;
                
                if (log.t_accept !== undefined) {
                    avgAccept += log.t_accept;
                    avgParse += log.t_parse;
                    avgRoute += log.t_route;
                    timingCount++;
                }

                // Flash Pipeline
                const pipeAccept = document.getElementById('pipe-accept');
                const pipeParse = document.getElementById('pipe-parse');
                const pipeRoute = document.getElementById('pipe-route');
                
                pipeAccept.classList.add('bg-blue-500/20');
                setTimeout(() => {
                    pipeAccept.classList.remove('bg-blue-500/20');
                    pipeParse.classList.add('bg-emerald-500/20');
                    setTimeout(() => {
                        pipeParse.classList.remove('bg-emerald-500/20');
                        pipeRoute.classList.add('bg-purple-500/20');
                        setTimeout(() => pipeRoute.classList.remove('bg-purple-500/20'), 150);
                    }, 150);
                }, 150);

            } catch (e) { console.error("Log parse error", e); }
        });
        
        while (logTailEl.children.length > 50) {
            logTailEl.removeChild(logTailEl.firstChild);
        }
        logTailEl.scrollTop = logTailEl.scrollHeight;

        methodChart.data.datasets[0].data = [methodCounts.GET, methodCounts.POST, methodCounts.OTHER];
        methodChart.update();
        
        latencyChart.data.datasets[0].data = latencyBuckets;
        latencyChart.update();
        
        if (timingCount > 0) {
            tAcceptEl.innerText = (avgAccept / timingCount).toFixed(0) + 'µs';
            tParseEl.innerText = (avgParse / timingCount).toFixed(0) + 'µs';
            tRouteEl.innerText = (avgRoute / timingCount).toFixed(0) + 'µs';
        }
    }
    
    // Proxy Topology
    if (data.proxy_nodes) {
        const container = document.getElementById('proxy-nodes-container');
        container.innerHTML = '';
        data.proxy_nodes.forEach((node, idx) => {
            const cached = proxyReqCache[node.host] || 0;
            const nodeReqs = node.req_count - cached;
            proxyReqCache[node.host] = node.req_count;
            
            const isHealthy = node.healthy;
            const bgClass = isHealthy ? 'bg-emerald-900 border-emerald-600 text-emerald-200' : 'bg-red-900 border-red-600 text-red-200';
            
            container.innerHTML += `
                <div class="flex flex-col items-center w-1/2 px-1 overflow-hidden">
                    <div class="${bgClass} border text-[10px] sm:text-xs px-2 py-1 rounded mb-1 shadow max-w-full truncate text-center w-full" title="${node.host}">Node ${idx + 1}</div>
                    <div class="text-[10px] text-gray-400 font-mono flex items-center gap-1 whitespace-nowrap">
                        <span>${nodeReqs} r/s</span>
                        <span class="text-blue-300">| ${node.latency_ms}ms</span>
                    </div>
                </div>
            `;
            
            // Wire logic
            const wire = document.getElementById('wire-node-' + idx);
            if (wire) {
                wire.setAttribute('stroke', isHealthy ? 'rgba(52, 211, 153, 0.4)' : 'rgba(248, 113, 113, 0.4)');
            }
            
            // Animation dot
            if (nodeReqs > 0 && isHealthy) {
                const anims = document.getElementById('proxy-animations');
                const dot = document.createElement('div');
                dot.className = 'absolute w-1.5 h-1.5 bg-blue-400 rounded-full shadow-[0_0_8px_#60a5fa] z-20';
                
                // Animate from Server (center) to Backend
                dot.style.top = '90px';
                dot.style.left = '50%';
                
                dot.animate([
                    { top: '90px', left: '50%' },
                    { top: '150px', left: idx === 0 ? '25%' : '75%' }
                ], { duration: 600, easing: 'ease-out' });
                
                anims.appendChild(dot);
                setTimeout(() => dot.remove(), 600);
            }
        });
    }
};

evtSource.onerror = function(err) {
    serverBadgeEl.className = "flex items-center space-x-3 bg-red-500/10 px-4 py-2 rounded-full border border-red-500/20 transition-colors";
    serverTextEl.innerText = "Server Offline";
    serverDotEl.className = "w-3 h-3 bg-red-500 rounded-full";
};

function triggerFlood() {
    const t = document.getElementById('slider-threads').value;
    const c = document.getElementById('slider-conns').value;
    fetch(`/api/chaos/flood?threads=${t}&conns=${c}`, { method: 'POST' });
}
function triggerSlowloris() {
    const t = document.getElementById('slider-threads').value;
    const c = document.getElementById('slider-conns').value;
    fetch(`/api/chaos/slowloris?threads=${t}&conns=${c}`, { method: 'POST' });
}
function triggerSmuggle() {
    fetch('/api/chaos/smuggle', { method: 'POST' });
}
function triggerShutdown() {
    fetch('/api/admin/shutdown', { method: 'POST' });
}
