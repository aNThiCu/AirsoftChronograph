document.addEventListener('DOMContentLoaded', () => {
    const gateway = `ws://${window.location.hostname}/ws`;
    let websocket;

    const statusIndicator = document.getElementById('status-indicator');
    const statusText = document.getElementById('status-text');
    const saveButton = document.getElementById('save-button');
    const bbWeightInput = document.getElementById('bb-weight');
    const sensorDistanceInput = document.getElementById('sensor-distance');
    const shotHistoryList = document.getElementById('shot-history-list');

    const chartSvg = document.getElementById('speed-chart');
    const maxChartElements = 10;
    let chartData = [];
    let shotCounter = 0;

    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }

    function onOpen(event) {
        console.log('Connection opened');
        statusIndicator.className = 'connected';
        statusText.textContent = 'Connected';
        shotCounter = 0; // Reset counter on new connection
        shotHistoryList.innerHTML = ''; // Clear history on new connection
    }

    function onClose(event) {
        console.log('Connection closed');
        statusIndicator.className = 'disconnected';
        statusText.textContent = 'Disconnected';
        setTimeout(initWebSocket, 2000);
    }

    function onMessage(event) {
        try {
            const data = JSON.parse(event.data);

            // Individual shot data
            if (data.hasOwnProperty('metric')) {
                shotCounter++;
                document.getElementById('metric').textContent = data.metric.toFixed(1);
                document.getElementById('joules').textContent = data.joules.toFixed(1);
                updateChart(data.metric);
                addShotToHistory(shotCounter, data.metric, data.joules);
            }

            // Burst summary data
            if (data.hasOwnProperty('rps')) {
                document.getElementById('rps').textContent = data.rps.toFixed(1);
            }
            if (data.hasOwnProperty('avg_metric')) {
                // Optionally display average speed somewhere
                console.log(`Burst Average Speed: ${data.avg_metric.toFixed(1)}`);
            }

            // Initial settings from ESP32
            if (data.hasOwnProperty('bbWeight')) {
                bbWeightInput.value = data.bbWeight;
            }
            if (data.hasOwnProperty('distanceAcross')) {
                sensorDistanceInput.value = data.distanceAcross;
            }

            // Debug messages
            if (data.hasOwnProperty('debug')) {
                console.log(`ESP32 Debug: ${data.debug}`);
            }

        } catch (e) {
            console.error('Error parsing message:', e);
        }
    }

    function addShotToHistory(count, speed, joules) {
        const listItem = document.createElement('li');
        listItem.textContent = `#${count}: ${speed.toFixed(1)} m/s, ${joules} J`;
        shotHistoryList.appendChild(listItem);
        // Auto-scroll to the bottom
        shotHistoryList.parentElement.scrollTop = shotHistoryList.parentElement.scrollHeight;
    }

    function sendConfig() {
        const config = {
            bbWeight: parseFloat(bbWeightInput.value),
            distanceAcross: parseInt(sensorDistanceInput.value)
        };
        websocket.send(JSON.stringify(config));
        console.log('Sent config:', config);
    }

    function updateChart(newValue) {
        if (newValue <= 0) return;

        chartData.push(newValue);
        if (chartData.length > maxChartElements) {
            chartData.shift(); // Remove the oldest element
        }
        renderChart();
    }

    function renderChart() {
        chartSvg.innerHTML = ''; // Clear previous chart
        const svgWidth = chartSvg.clientWidth;
        const svgHeight = chartSvg.clientHeight;
        const barWidth = svgWidth / chartData.length;
        const maxVal = Math.max(...chartData, 1); // Avoid division by zero
        const topMargin = 20; // Space for text labels above bars
        const availableHeight = svgHeight - topMargin;

        chartData.forEach((value, i) => {
            const barHeight = (value / maxVal) * availableHeight;
            
            const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
            rect.setAttribute('x', i * barWidth);
            rect.setAttribute('y', svgHeight - barHeight);
            rect.setAttribute('width', barWidth - 2);
            rect.setAttribute('height', barHeight);

            const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
            text.setAttribute('x', i * barWidth + (barWidth / 2));
            text.setAttribute('y', svgHeight - barHeight - 5);
            text.textContent = value.toFixed(1);

            chartSvg.appendChild(rect);
            chartSvg.appendChild(text);
        });
    }

    saveButton.addEventListener('click', sendConfig);

    initWebSocket();
});