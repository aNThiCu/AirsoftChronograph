document.addEventListener('DOMContentLoaded', () => {
    const SERVICE_UUID = "4fa8f700-4323-40f9-a961-68f1488d8a1e";
    const CHAR_SHOT_UUID = "4fa8f701-4323-40f9-a961-68f1488d8a1e";
    const CHAR_CONFIG_UUID = "4fa8f702-4323-40f9-a961-68f1488d8a1e";

    let bleDevice, shotCharacteristic, configCharacteristic;

    const connectBtn = document.getElementById('connect-btn');
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

    connectBtn.addEventListener('click', async () => {
        try {
            statusText.textContent = 'Connecting...';
            bleDevice = await navigator.bluetooth.requestDevice({
                filters: [{ name: 'ESP32-Chronograph' }],
                optionalServices: [SERVICE_UUID]
            });

            bleDevice.addEventListener('gattserverdisconnected', onDisconnected);

            const server = await bleDevice.gatt.connect();
            const service = await server.getPrimaryService(SERVICE_UUID);

            shotCharacteristic = await service.getCharacteristic(CHAR_SHOT_UUID);
            configCharacteristic = await service.getCharacteristic(CHAR_CONFIG_UUID);

            await shotCharacteristic.startNotifications();
            shotCharacteristic.addEventListener('characteristicvaluechanged', handleShotNotification);

            statusIndicator.className = 'connected';
            statusText.textContent = 'Connected';
            connectBtn.style.display = 'none';
            shotCounter = 0;
            shotHistoryList.innerHTML = '';
        } catch (error) {
            console.error('BLE Error:', error);
            statusIndicator.className = 'disconnected';
            statusText.textContent = 'Disconnected';
        }
    });

    function onDisconnected() {
        statusIndicator.className = 'disconnected';
        statusText.textContent = 'Disconnected';
        connectBtn.style.display = 'inline-block';
    }

    function handleShotNotification(event) {
        const value = event.target.value;
        
        // Single Shot Packet (4 bytes)
        if (value.byteLength === 4) {
            shotCounter++;
            const speed = (value.getUint8(0) << 8 | value.getUint8(1)) / 10.0;
            const joules = (value.getUint8(2) << 8 | value.getUint8(3)) / 100.0;

            document.getElementById('metric').textContent = speed.toFixed(1);
            document.getElementById('joules').textContent = joules.toFixed(2);
            document.getElementById('rps').textContent = '0.0';

            updateChart(speed);
            addShotToHistory(shotCounter, speed, joules);
        }
    }

    async function sendConfig() {
        if (!configCharacteristic) return;

        const weightMg = Math.round(parseFloat(bbWeightInput.value) * 1000);
        const distanceMm = parseInt(sensorDistanceInput.value, 10);

        const buffer = new ArrayBuffer(3);
        const dataView = new DataView(buffer);
        dataView.setUint16(0, weightMg, false); // Big-Endian
        dataView.setUint8(2, distanceMm);

        await configCharacteristic.writeValue(buffer);
        console.log('BLE Config Sent');
    }

    function addShotToHistory(count, speed, joules) {
        const listItem = document.createElement('li');
        listItem.textContent = `#${count}: ${speed.toFixed(1)} m/s, ${joules.toFixed(2)} J`;
        shotHistoryList.appendChild(listItem);
        shotHistoryList.parentElement.scrollTop = shotHistoryList.parentElement.scrollHeight;
    }

    function updateChart(newValue) {
        if (newValue <= 0) return;
        chartData.push(newValue);
        if (chartData.length > maxChartElements) {
            chartData.shift();
        }
        renderChart();
    }

    function renderChart() {
        chartSvg.innerHTML = '';
        const svgWidth = chartSvg.clientWidth;
        const svgHeight = chartSvg.clientHeight;
        const barWidth = svgWidth / chartData.length;
        const maxVal = Math.max(...chartData, 1);
        const topMargin = 20;
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
});