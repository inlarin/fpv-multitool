#include "web_ui.h"
#include <Arduino.h>

const char WEB_INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FPV MultiTool</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='6' fill='%230f0f1e'/><path d='M18 3 L7 18 h6 l-3 11 L25 13 h-6 l3-10z' fill='%230cf'/></svg>">
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: #0f0f1e; color: #e0e0e0;
  font-family: -apple-system,BlinkMacSystemFont,sans-serif;
  max-width: 1700px;      /* lets 3 card columns fit on 2K after container padding + grid gaps */
  margin: 0 auto;
  padding: clamp(8px, 2vw, 20px);
}
h1 {
  color: #0af;
  font-size: clamp(18px, 2.4vw, 26px);
  margin-bottom: 10px;
  text-align: center;
  padding: 0 90px;        /* keep title clear of the fixed #connStatus badge on narrow viewports */
}
.tabs { display: flex; justify-content: center; flex-wrap: wrap; gap: 4px; margin-bottom: 15px; }
.tab { padding: 10px 14px; background: #1a1a2e; border-radius: 6px; cursor: pointer; white-space: nowrap; font-size: 14px; user-select: none; }
.tab.active { background: #0066aa; color: #fff; }
/* Cards flow into 2–3 columns on wide screens, single column on phones.
   Cap track width so lone cards don't stretch to 1200px on 4K monitors. */
.tab-content { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 320px), 520px)); gap: 12px; align-items: start; justify-content: center; }
.card { background: #1a1a2e; border-radius: 8px; padding: 16px; min-width: 0; }
.card h2 { color: #0cf; font-size: clamp(14px, 1.4vw, 17px); margin-bottom: 12px; }
.row { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; border-bottom: 1px solid #222; }
.row:last-child { border-bottom: none; }
.label { color: #888; font-size: 13px; }
.value { color: #fff; font-size: 15px; font-weight: 600; }
.big { font-size: 32px; color: #0cf; text-align: center; margin: 10px 0; }
input[type=range] { width: 100%; margin: 8px 0; }
input[type=number], input[type=text], input[type=password] { width: 100%; padding: 8px; background: #0a0a14; border: 1px solid #333; border-radius: 4px; color: #fff; font-size: 14px; margin: 4px 0; }
button { padding: 10px 16px; background: #0066aa; color: #fff; border: none; border-radius: 6px; cursor: pointer; font-size: 14px; margin: 4px 2px; }
button.danger { background: #aa2222; }
button.success { background: #22aa44; }
button:disabled { background: #444; cursor: not-allowed; }
.status { display: inline-block; padding: 4px 10px; border-radius: 12px; font-size: 12px; font-weight: bold; }
.status.on { background: #2a4; color: #fff; }
.status.off { background: #666; color: #ccc; }
.status.warn { background: #c82; color: #fff; }
.bar { height: 20px; background: #0a0a14; border-radius: 4px; overflow: hidden; margin: 8px 0; }
.bar-fill { height: 100%; background: linear-gradient(90deg,#2a4,#0af); transition: width 0.2s; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.warning { color: #f66; font-size: 12px; padding: 8px; background: #2a0a0a; border-radius: 4px; margin: 8px 0; }
.ok { color: #6f6; }
#connStatus { position: fixed; top: 10px; right: 10px; font-size: 12px; padding: 4px 8px; border-radius: 12px; }
#connStatus.connected { background: #2a4; color: #fff; }
#connStatus.disconnected { background: #a42; color: #fff; }
</style>
</head>
<body>
<div id="connStatus" class="disconnected">...</div>
<h1>FPV MultiTool</h1>

<div class="tabs">
  <div class="tab active" onclick="showTab('servo')">Servo</div>
  <div class="tab" onclick="showTab('motor')">Motor</div>
  <div class="tab" onclick="showTab('battery')">Battery</div>
  <div class="tab" onclick="showTab('elrs')">ELRS Flash</div>
  <div class="tab" onclick="showTab('crsf')">CRSF</div>
  <div class="tab" onclick="showTab('sys')">System</div>
</div>

<!-- ===== SERVO ===== -->
<div id="tab-servo" class="tab-content">
  <div class="card">
    <h2>Servo Tester (GPIO 2)</h2>
    <div class="big" id="servoUs">1500 μs</div>
    <input type="range" id="servoSlider" min="500" max="2500" value="1500" step="10" oninput="onServo()">
    <div class="grid">
      <button onclick="setServo(500)">Min 500</button>
      <button onclick="setServo(1500)">Center</button>
    </div>
    <div class="grid">
      <button onclick="setServo(2500)">Max 2500</button>
      <button class="danger" onclick="servoStop()">STOP</button>
    </div>
    <div class="row"><span class="label">Frequency:</span>
      <span>
        <button onclick="setServoFreq(50)">50 Hz</button>
        <button onclick="setServoFreq(330)">330 Hz</button>
      </span>
    </div>
    <div class="row"><span class="label">Sweep mode:</span>
      <button onclick="toggleSweep()"><span id="sweepBtn">Start</span></button>
    </div>
  </div>
</div>

<!-- ===== MOTOR ===== -->
<div id="tab-motor" class="tab-content" style="display:none">
  <div class="card">
    <h2>Motor Tester DShot (GPIO 2)</h2>
    <div class="warning">⚠ СНИМИ ПРОПЕЛЛЕРЫ перед тестом!</div>
    <div class="row"><span class="label">Status:</span><span id="motorStatus" class="status off">DISARMED</span></div>
    <div class="row"><span class="label">Protocol:</span>
      <select id="dshotSpeed" onchange="setDshotSpeed()" style="padding:6px;background:#0a0a14;color:#fff;border:1px solid #333;">
        <option value="150">DShot150</option>
        <option value="300" selected>DShot300</option>
        <option value="600">DShot600</option>
      </select>
    </div>
    <div class="big"><span id="throttleVal">0</span> <span style="color:#888;font-size:18px">/ 2000</span></div>
    <div class="bar"><div class="bar-fill" id="throttleBar" style="width:0%"></div></div>
    <input type="range" id="throttleSlider" min="0" max="2000" value="0" disabled oninput="onThrottle()">
    <div class="grid">
      <button class="success" id="armBtn" onclick="motorArm()">ARM</button>
      <button class="danger" onclick="motorDisarm()">DISARM</button>
    </div>
    <div class="grid">
      <button onclick="motorBeep()">Beep ESC</button>
      <button onclick="setThrottle(0)">Zero</button>
    </div>
    <div style="margin-top:14px;padding-top:10px;border-top:1px solid #333;">
      <div class="row">
        <span class="label">Max throttle:</span>
        <span class="value"><span id="maxThrottleVal">2000</span> / 2000</span>
      </div>
      <input type="range" id="maxThrottleSlider" min="100" max="2000" step="100" value="2000" oninput="onMaxThrottle()">
      <div class="row" style="margin-top:8px"><span class="label">Direction (armed):</span></div>
      <div class="grid">
        <button onclick="motorDirCW()">Dir CW</button>
        <button onclick="motorDirCCW()">Dir CCW</button>
      </div>
      <div class="row" style="margin-top:8px"><span class="label">3D mode (armed):</span></div>
      <div class="grid">
        <button onclick="motor3DOn()">3D ON</button>
        <button onclick="motor3DOff()">3D OFF</button>
      </div>
    </div>
  </div>
</div>

<!-- ===== BATTERY ===== -->
<div id="tab-battery" class="tab-content" style="display:none">
  <div class="card">
    <h2>DJI Battery (I2C 0x0B)</h2>
    <div class="row">
      <span class="label">Connected:</span>
      <span>
        <span id="battConn" class="status off">NO</span>
        <span id="battSealStatus" class="status warn" style="display:none">SEALED</span>
      </span>
    </div>
    <div class="big" id="battSOC">-- %</div>
    <div class="bar"><div class="bar-fill" id="battBar" style="width:0%"></div></div>
    <div class="grid">
      <div class="row"><span class="label">Voltage:</span><span class="value" id="battVolt">-</span></div>
      <div class="row"><span class="label">Current:</span><span class="value" id="battCurr">-</span></div>
      <div class="row"><span class="label">Temp:</span><span class="value" id="battTemp">-</span></div>
      <div class="row"><span class="label">Cycles:</span><span class="value" id="battCycle">-</span></div>
      <div class="row"><span class="label">Capacity:</span><span class="value" id="battCap">-</span></div>
      <div class="row"><span class="label">Design:</span><span class="value" id="battDesign">-</span></div>
    </div>
  </div>

  <div class="card">
    <h2>Cells (<span id="cellsSync">async</span>)</h2>
    <div id="cellsGrid"></div>
    <div class="row"><span class="label">Delta:</span><span class="value" id="cellDelta">-</span></div>
    <div class="row"><span class="label">Pack V (sync):</span><span class="value" id="packV">-</span></div>
  </div>

  <div class="card">
    <h2>Status Registers</h2>
    <div class="row">
      <span class="label">Operation:</span>
      <span class="value" id="opStatus" style="font-family:monospace;font-size:11px">-</span>
    </div>
    <div class="row" style="color:#aaa;font-size:11px"><span id="opDecoded">-</span></div>
    <div class="row">
      <span class="label">Safety:</span>
      <span class="value" id="safetyStatus" style="font-family:monospace;font-size:11px">-</span>
    </div>
    <div class="row" style="color:#aaa;font-size:11px"><span id="safetyDecoded">-</span></div>
    <div class="row">
      <span class="label">PF Status:</span>
      <span class="value" id="pfStatus" style="font-family:monospace;font-size:11px">-</span>
    </div>
    <div class="row" style="font-size:11px"><span id="pfDecoded">-</span></div>
    <div class="row">
      <span class="label">Mfg Status:</span>
      <span class="value" id="mfgStatus" style="font-family:monospace;font-size:11px">-</span>
    </div>
    <div class="row" style="color:#aaa;font-size:11px"><span id="mfgDecoded">-</span></div>
  </div>

  <div class="card">
    <h2>Battery Info</h2>
    <div class="row"><span class="label">Model:</span><span class="value" id="battModel" style="color:#f0f">-</span></div>
    <div class="row"><span class="label">Manufacturer:</span><span class="value" id="battMfr">-</span></div>
    <div class="row"><span class="label">Device:</span><span class="value" id="battDev">-</span></div>
    <div class="row"><span class="label">Chemistry:</span><span class="value" id="battChem">-</span></div>
    <div class="row"><span class="label">Serial:</span><span class="value" id="battSN">-</span></div>
    <div class="row"><span class="label">Chip:</span><span class="value" id="battChip">-</span></div>
    <div class="row"><span class="label">FW / HW:</span><span class="value" id="battFwHw">-</span></div>
    <div id="keyWarning" class="warning" style="display:none">
      ⚠ Эта модель требует DJI per-pack ключ.<br>
      Публичные ключи отсутствуют. Unseal/Clear PF работать не будут.
    </div>
  </div>

  <div class="card">
    <h2>Service (advanced)</h2>
    <div class="warning">⚠ Требует unseal. Для Mavic 2+/Mavic 3/Mavic 4 ключи DJI не опубликованы.</div>
    <div class="grid">
      <button onclick="battAction('unseal')">Unseal</button>
      <button onclick="battAction('clearpf')">Clear PF</button>
    </div>
    <div class="grid">
      <button onclick="battAction('seal')">Seal</button>
      <button onclick="battAction('reset')">Soft Reset</button>
    </div>
    <button class="danger" onclick="battAction('fullservice')" style="width:100%;margin-top:4px;">Full Service (unseal + clear + seal)</button>
    <div id="battActionResult" style="margin-top:10px;color:#ff0"></div>
  </div>
</div>

<!-- ===== ELRS FLASH ===== -->
<div id="tab-elrs" class="tab-content" style="display:none">
  <div class="card">
    <h2>ELRS Receiver Flasher</h2>
    <div class="warning">
      <b>Инструкция:</b><br>
      1. Подключи приёмник: <b>RX→GPIO43, TX→GPIO44, GND, 5V</b><br>
      2. Переведи приёмник в <b>DFU режим</b> (зажми кнопку на нём и подай питание)<br>
      3. Загрузи .bin файл ниже и нажми Flash
    </div>
    <div class="row"><span class="label">Firmware (.bin / .bin.gz / .elrs):</span></div>
    <input type="file" id="fwFile" accept=".bin,.gz,.elrs" onchange="onFwSelect()" style="margin:8px 0; width:100%;">
    <div class="row"><span class="label">Размер:</span><span class="value" id="fwSize">-</span></div>
    <div class="row"><span class="label">Статус:</span><span class="value" id="flashStage">idle</span></div>
    <div class="bar"><div class="bar-fill" id="flashBar" style="width:0%;background:#fa0"></div></div>
    <div class="grid">
      <button class="success" id="uploadBtn" onclick="uploadFw()" disabled>Upload</button>
      <button id="flashBtn" onclick="startFlash()" disabled>Flash!</button>
    </div>
    <button class="danger" onclick="clearFw()" style="width:100%;margin-top:4px;">Clear</button>
    <div id="flashResult" style="margin-top:10px; color:#0f0;"></div>
  </div>
</div>

<!-- ===== CRSF ===== -->
<div id="tab-crsf" class="tab-content" style="display:none">
  <div class="card">
    <h2>CRSF / ELRS Telemetry</h2>
    <div class="warning">
      Подключи RX/TX приёмника к GPIO44/43 (ESP UART), 5V, GND.<br>
      Для инвертированного CRSF (F3/F4 FC) поставь галку.
    </div>
    <div class="row">
      <span class="label">Status:</span>
      <span>
        <span id="crsfStatus" class="status off">OFF</span>
        <span id="crsfConnected" class="status off">NO LINK</span>
      </span>
    </div>
    <div class="row">
      <span class="label">Inverted:</span>
      <span><input type="checkbox" id="crsfInverted"></span>
    </div>
    <div class="grid">
      <button class="success" onclick="crsfStart()">Start</button>
      <button class="danger" onclick="crsfStop()">Stop</button>
    </div>
  </div>

  <div class="card">
    <h2>Link Stats</h2>
    <div class="row"><span class="label">Uplink RSSI:</span><span class="value" id="crsfRssi">- dBm</span></div>
    <div class="row"><span class="label">Link Quality:</span><span class="value" id="crsfLQ">- %</span></div>
    <div class="bar"><div class="bar-fill" id="crsfLqBar" style="width:0%"></div></div>
    <div class="row"><span class="label">SNR:</span><span class="value" id="crsfSnr">- dB</span></div>
    <div class="row"><span class="label">RF Mode:</span><span class="value" id="crsfRf">-</span></div>
    <div class="row"><span class="label">TX Power:</span><span class="value" id="crsfPower">-</span></div>
    <div class="row"><span class="label">Downlink:</span><span class="value" id="crsfDl">-</span></div>
    <div class="row"><span class="label">Frames/Errors:</span><span class="value" id="crsfFrames">0 / 0</span></div>
  </div>

  <div class="card">
    <h2>Channels</h2>
    <div id="channelsGrid"></div>
  </div>

  <div class="card">
    <h2>FC Telemetry</h2>
    <div class="row"><span class="label">Flight Mode:</span><span class="value" id="crsfMode">-</span></div>
    <div class="row"><span class="label">Battery:</span><span class="value" id="crsfBatt">-</span></div>
    <div class="row"><span class="label">Attitude:</span><span class="value" id="crsfAtt">-</span></div>
    <div class="row"><span class="label">GPS:</span><span class="value" id="crsfGPS">-</span></div>
  </div>

  <div class="card">
    <h2>Device Info</h2>
    <div class="row"><span class="label">Name:</span><span class="value" id="crsfDevName">-</span></div>
    <div class="row"><span class="label">FW:</span><span class="value" id="crsfDevFw">-</span></div>
    <div class="row"><span class="label">Serial:</span><span class="value" id="crsfDevSerial">-</span></div>
    <div class="row"><span class="label">Parameters:</span><span class="value" id="crsfDevFields">-</span></div>
    <div class="grid">
      <button onclick="crsfPing()">Ping Device</button>
      <button onclick="crsfReadParams()">Read Params</button>
    </div>
  </div>

  <div class="card">
    <h2>Parameters</h2>
    <div id="crsfParams">No parameters loaded. Click "Read Params" above.</div>
  </div>

  <div class="card">
    <h2>Commands</h2>
    <div class="warning">⚠ Команды шлются на приёмник по адресу 0xEC</div>
    <div class="grid">
      <button class="danger" onclick="crsfBind()">Bind Mode</button>
      <button class="danger" onclick="crsfReboot()">Reboot RX</button>
    </div>
    <div class="warning" style="margin-top:10px;">
      Для смены <b>bind phrase</b> — запусти WiFi mode на приёмнике,
      подключись к его AP <code>ExpressLRS RX</code> (pass <code>expresslrs</code>)
      и открой <code>http://10.0.0.1</code>. Требует сначала <b>Read Params</b>.
    </div>
    <div class="grid">
      <button onclick="crsfEnterWifi()">Enter WiFi Update Mode</button>
    </div>
    <div id="crsfCmdResult" style="margin-top:10px;color:#ff0;"></div>
  </div>
</div>

<!-- ===== SYSTEM ===== -->
<div id="tab-sys" class="tab-content" style="display:none">
  <div class="card">
    <h2>System Info</h2>
    <div class="row"><span class="label">IP:</span><span class="value" id="sysIP">-</span></div>
    <div class="row"><span class="label">Uptime:</span><span class="value" id="sysUptime">-</span></div>
    <div class="row"><span class="label">Free heap:</span><span class="value" id="sysHeap">-</span></div>
    <div class="row"><span class="label">WiFi clients:</span><span class="value" id="sysClients">-</span></div>
  </div>
  <div class="card">
    <h2>WiFi Config</h2>
    <div style="margin-bottom:8px">
      <button onclick="scanWifi()" id="wifiScanBtn">Scan networks</button>
      <span id="wifiScanStatus" style="color:#888;margin-left:8px;font-size:12px"></span>
    </div>
    <div id="wifiScanList" style="margin-bottom:10px"></div>
    <div class="label">SSID:</div>
    <input type="text" id="wifiSSID" placeholder="your WiFi name">
    <div class="label">Password:</div>
    <input type="password" id="wifiPASS" placeholder="password">
    <button onclick="saveWifi()">Save & Connect STA</button>
    <button class="danger" onclick="clearWifi()">Clear (back to AP)</button>
  </div>
</div>

<script>
let ws = null, wsOk = false;

function connStatus(ok) {
  wsOk = ok;
  const el = document.getElementById('connStatus');
  el.className = ok ? 'connected' : 'disconnected';
  el.textContent = ok ? '● Connected' : '● Disconnected';
}

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => connStatus(true);
  ws.onclose = () => { connStatus(false); setTimeout(connect, 2000); };
  ws.onerror = () => ws.close();
  ws.onmessage = (e) => {
    try { handleMsg(JSON.parse(e.data)); } catch(err) {}
  };
}

function send(obj) {
  if (ws && wsOk) ws.send(JSON.stringify(obj));
  else fetch('/api', {method:'POST', body:JSON.stringify(obj)});
}

function showTab(name) {
  document.querySelectorAll('.tab-content').forEach(e => e.style.display = 'none');
  // CSS default is grid (multi-column on wide screens) — restore it, don't force block.
  document.getElementById('tab-'+name).style.display = '';
  document.querySelectorAll('.tab').forEach(e => e.classList.remove('active'));
  event.target.classList.add('active');
}

// === SERVO ===
function onServo() {
  const us = +document.getElementById('servoSlider').value;
  document.getElementById('servoUs').textContent = us + ' μs';
  send({cmd:'servo', us});
}
function setServo(us) {
  document.getElementById('servoSlider').value = us;
  onServo();
}
function setServoFreq(hz) { send({cmd:'servoFreq', hz}); }
function servoStop() { send({cmd:'servoStop'}); }
let sweeping = false;
function toggleSweep() {
  sweeping = !sweeping;
  document.getElementById('sweepBtn').textContent = sweeping ? 'Stop' : 'Start';
  send({cmd:'servoSweep', on: sweeping});
}

// === MOTOR ===
let armed = false;
function motorArm() {
  send({cmd:'motorArm'});
}
function motorDisarm() {
  send({cmd:'motorDisarm'});
  document.getElementById('throttleSlider').value = 0;
  onThrottle();
}
function onThrottle() {
  const t = +document.getElementById('throttleSlider').value;
  document.getElementById('throttleVal').textContent = t;
  document.getElementById('throttleBar').style.width = (t/2000*100) + '%';
  send({cmd:'throttle', value: t});
}
function setThrottle(v) { document.getElementById('throttleSlider').value = v; onThrottle(); }
function setDshotSpeed() {
  send({cmd:'dshotSpeed', speed: +document.getElementById('dshotSpeed').value});
}
function motorBeep() { send({cmd:'motorBeep'}); }
function onMaxThrottle() {
  const v = +document.getElementById('maxThrottleSlider').value;
  document.getElementById('maxThrottleVal').textContent = v;
  const slider = document.getElementById('throttleSlider');
  slider.max = v;
  if (+slider.value > v) { slider.value = v; onThrottle(); }
  send({cmd:'motorMaxThrottle', value: v});
}
function motorDirCW()  { if (confirm('Set direction CW (normal)? ESC must be armed.'))  send({cmd:'motorDirCW'}); }
function motorDirCCW() { if (confirm('Set direction CCW (reverse)? ESC must be armed.')) send({cmd:'motorDirCCW'}); }
function motor3DOn()   { if (confirm('Enable 3D mode? ESC must be armed.'))  send({cmd:'motor3DOn'}); }
function motor3DOff()  { if (confirm('Disable 3D mode? ESC must be armed.')) send({cmd:'motor3DOff'}); }

// === BATTERY ===
function battAction(action) {
  const confirmMsgs = {
    unseal:      'Unseal battery? Tries TI default key.\nMavic 2+/3/4 will likely fail (no public key).',
    clearpf:     'Clear Permanent Failure flags?\nBattery MUST be unsealed first.\nOperation can take 2-3 seconds.',
    seal:        'Seal battery (lock config)?',
    reset:       'Send soft reset to BMS?',
    fullservice: 'FULL SERVICE: unseal + clear PF + seal.\nPotentially destructive. Continue only if you know what you are doing.'
  };
  if (confirmMsgs[action] && !confirm(confirmMsgs[action])) return;
  document.getElementById('battActionResult').textContent = '...';
  fetch('/api/batt?action='+action).then(r=>r.text()).then(t=>{
    document.getElementById('battActionResult').textContent = t;
  });
}

// === CRSF ===
function crsfStart() {
  const inv = document.getElementById('crsfInverted').checked ? '1' : '0';
  fetch('/api/crsf/start?inverted='+inv, {method:'POST'})
    .then(r=>r.text()).then(t=>showCmdResult(t));
}
function crsfStop()  { fetch('/api/crsf/stop',  {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function crsfPing()  { fetch('/api/crsf/ping',  {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function crsfReadParams() { fetch('/api/crsf/params', {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function crsfBind()   { if (confirm('Send bind command?')) fetch('/api/crsf/bind',   {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function crsfReboot() { if (confirm('Reboot receiver?'))   fetch('/api/crsf/reboot', {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function crsfEnterWifi() {
  if (!confirm('Switch RX to WiFi update mode? Link will drop; RX will start its own AP.')) return;
  fetch('/api/crsf/wifi', {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t));
}

// Render CRSF parameter tree: group by parent_id, folders expand, commands are state-aware.
// ELRS COMMAND status codes: 0=READY, 1=START, 2=PROGRESS, 3=CONFIRM_NEEDED, 4=CONFIRM, 5=CANCEL, 6=QUERY.
function renderCrsfCommand(p) {
  const info = p.info || '';
  const s = p.status | 0;
  if (s === 2) { // PROGRESS
    return `<span style="color:#fc0">${info || 'Executing...'}</span> `
         + `<button onclick="crsfWriteParam(${p.id}, 5)">Abort</button>`;
  }
  if (s === 3) { // CONFIRMATION_NEEDED
    return `<span style="color:#f80">${info || 'Confirm?'}</span> `
         + `<button onclick="crsfWriteParam(${p.id}, 4)">Confirm</button> `
         + `<button onclick="crsfWriteParam(${p.id}, 5)">Cancel</button>`;
  }
  if (s === 4) return `<span style="color:#0f0">Done</span>`;
  if (s === 5) return `<span style="color:#888">Cancelled</span>`;
  // READY (0) / QUERY (6) / unknown → show Execute button
  return `<button onclick="crsfWriteParam(${p.id}, 1)">Execute</button>`
       + (info ? ` <span style="color:#888;font-size:11px">${info}</span>` : '');
}

function renderCrsfParamValue(p) {
  if (p.type === 9) { // TEXT_SELECTION
    const opts = (p.opts || '').split(';');
    let sel = `<select onchange="crsfWriteParam(${p.id}, this.value)" style="background:#0a0a14;color:#fff;border:1px solid #333;padding:4px;">`;
    opts.forEach((o,i) => {
      sel += `<option value="${i}"${i === p.idx ? ' selected' : ''}>${o}</option>`;
    });
    return sel + '</select>';
  }
  if (p.type === 10) return `<input type="text" value="${p.value || ''}" onchange="crsfWriteParam(${p.id}, this.value)" style="background:#0a0a14;color:#fff;border:1px solid #333;padding:4px;width:120px;">`;
  if (p.type === 12) return `<span style="color:#aaa">${p.value || ''}</span>`;
  if (p.type === 13) return renderCrsfCommand(p);
  // Numeric
  return `<input type="number" value="${p.value}" min="${p.min||0}" max="${p.max||255}" onchange="crsfWriteParam(${p.id}, this.value)" style="background:#0a0a14;color:#fff;border:1px solid #333;padding:4px;width:80px;">`;
}

function renderCrsfParams(params) {
  // Index by id, bucket children by parent
  const byId = {};
  const children = {};
  params.forEach(p => {
    byId[p.id] = p;
    const pp = p.parent | 0;
    (children[pp] = children[pp] || []).push(p);
  });

  // Depth-first rendering with indentation
  function renderGroup(parentId, depth) {
    const list = children[parentId] || [];
    let html = '';
    list.forEach(p => {
      const indent = depth * 14;
      const nameCol = `<b>${p.name}</b>` + (p.unit ? ` <span style="color:#888">(${p.unit})</span>` : '');
      if (p.type === 11) {
        // Folder header
        html += `<div class="row" style="padding-left:${indent}px;background:#1a1a2a;margin-top:4px;border-left:3px solid #55a;">`
              + `<span style="color:#8af">▾ ${p.name}</span></div>`;
        html += renderGroup(p.id, depth + 1);
      } else {
        html += `<div class="row" style="padding-left:${indent}px;">`
              + `<span class="label" style="flex:1">${nameCol}</span>`
              + `<span>${renderCrsfParamValue(p)}</span></div>`;
      }
    });
    return html;
  }

  // Roots: anything whose parent is 0 or not in byId
  // (ELRS uses parent_id=0 for root-level params)
  let html = renderGroup(0, 0);
  // Orphans — params pointing to an unknown parent
  Object.keys(children).forEach(pid => {
    const p = pid | 0;
    if (p !== 0 && !byId[p]) html += renderGroup(p, 0);
  });
  return html || '<div style="color:#888">No visible parameters.</div>';
}
function crsfWriteParam(id, value) {
  fetch(`/api/crsf/write?id=${id}&value=${encodeURIComponent(value)}`, {method:'POST'})
    .then(r=>r.text()).then(t=>showCmdResult(t));
}
function showCmdResult(msg) {
  const el = document.getElementById('crsfCmdResult');
  if (el) { el.textContent = msg; setTimeout(()=>el.textContent='', 3000); }
}

// === ELRS FLASH ===
let selectedFile = null;
function onFwSelect() {
  selectedFile = document.getElementById('fwFile').files[0];
  if (selectedFile) {
    document.getElementById('fwSize').textContent = (selectedFile.size/1024).toFixed(1) + ' KB';
    document.getElementById('uploadBtn').disabled = false;
  }
}
function uploadFw() {
  if (!selectedFile) return;
  const fd = new FormData();
  fd.append('firmware', selectedFile);
  document.getElementById('flashStage').textContent = 'Uploading...';
  document.getElementById('uploadBtn').disabled = true;
  fetch('/api/flash/upload', {method:'POST', body:fd})
    .then(r=>r.text()).then(t=>{
      document.getElementById('flashStage').textContent = 'Uploaded';
      document.getElementById('flashBtn').disabled = false;
      document.getElementById('flashResult').textContent = t;
    })
    .catch(e=>{
      document.getElementById('flashStage').textContent = 'Upload error';
      document.getElementById('flashResult').textContent = e;
      document.getElementById('uploadBtn').disabled = false;
    });
}
function startFlash() {
  if (!confirm('Убедись что приёмник в DFU режиме!\nПродолжить прошивку?')) return;
  document.getElementById('flashBtn').disabled = true;
  document.getElementById('flashResult').textContent = '';
  fetch('/api/flash/start', {method:'POST'})
    .then(r=>r.text()).then(t=>{
      document.getElementById('flashResult').textContent = t;
    });
}
function clearFw() {
  fetch('/api/flash/clear', {method:'POST'}).then(()=>{
    document.getElementById('fwFile').value = '';
    document.getElementById('fwSize').textContent = '-';
    document.getElementById('flashStage').textContent = 'idle';
    document.getElementById('flashBar').style.width = '0%';
    document.getElementById('flashResult').textContent = '';
    document.getElementById('uploadBtn').disabled = true;
    document.getElementById('flashBtn').disabled = true;
    selectedFile = null;
  });
}

// === WiFi ===
function saveWifi() {
  const ssid = document.getElementById('wifiSSID').value;
  const pass = document.getElementById('wifiPASS').value;
  if (!ssid) return alert('Enter SSID');
  fetch('/api/wifi/save', {method:'POST', body:JSON.stringify({ssid,pass})})
    .then(r=>r.text()).then(t=>alert(t));
}
function clearWifi() {
  if (!confirm('Reset WiFi to AP mode?')) return;
  fetch('/api/wifi/clear').then(r=>r.text()).then(t=>alert(t));
}

// Scan is async on the ESP side — HTTP returns right away, we poll for results.
// Expect a 1-3s AP hiccup during the scan; WebSocket keepalive rides it out.
function scanWifi() {
  const btn = document.getElementById('wifiScanBtn');
  const stat = document.getElementById('wifiScanStatus');
  const list = document.getElementById('wifiScanList');
  btn.disabled = true;
  stat.textContent = 'scanning... (brief AP lag)';
  list.innerHTML = '';
  fetch('/api/wifi/scan', {method:'POST'})
    .then(() => pollWifiScan(0))
    .catch(e => { stat.textContent = 'scan error'; btn.disabled = false; });
}
function pollWifiScan(tries) {
  if (tries > 30) {  // ~15s cap
    document.getElementById('wifiScanStatus').textContent = 'timeout';
    document.getElementById('wifiScanBtn').disabled = false;
    return;
  }
  setTimeout(() => {
    fetch('/api/wifi/scan_results').then(r=>r.json()).then(j => {
      if (!j.done) return pollWifiScan(tries + 1);
      renderWifiScan(j.nets || []);
      document.getElementById('wifiScanStatus').textContent = (j.nets||[]).length + ' networks';
      document.getElementById('wifiScanBtn').disabled = false;
    }).catch(() => pollWifiScan(tries + 1));
  }, 500);
}
function renderWifiScan(nets) {
  nets.sort((a,b) => b.rssi - a.rssi);
  const list = document.getElementById('wifiScanList');
  if (!nets.length) { list.innerHTML = '<div style="color:#888">no networks found</div>'; return; }
  list.innerHTML = nets.map(n => {
    const bars = n.rssi > -55 ? '▂▄▆█' : n.rssi > -70 ? '▂▄▆_' : n.rssi > -80 ? '▂▄__' : '▂___';
    const lock = n.enc ? '🔒' : ' ';
    const ssid = (n.ssid || '<hidden>').replace(/</g,'&lt;');
    return `<div class="row" style="cursor:pointer;padding:4px;border-bottom:1px solid #222" onclick="pickSsid(${JSON.stringify(n.ssid||'').replace(/"/g,'&quot;')})">`
         + `<span style="flex:1">${lock} ${ssid}</span>`
         + `<span style="color:#8af;font-family:monospace">${bars}</span>`
         + `<span style="color:#888;margin-left:8px;width:60px;text-align:right">${n.rssi} dBm</span>`
         + `<span style="color:#666;margin-left:8px;width:30px;text-align:right">ch${n.ch}</span>`
         + `</div>`;
  }).join('');
}
function pickSsid(ssid) {
  document.getElementById('wifiSSID').value = ssid;
  document.getElementById('wifiPASS').focus();
}

// === Incoming messages ===
function handleMsg(m) {
  if (m.type === 'batt') {
    const c = m.connected;
    const bc = document.getElementById('battConn');
    bc.className = 'status ' + (c ? 'on' : 'off');
    bc.textContent = c ? 'YES' : 'NO';
    if (!c) return;

    document.getElementById('battSOC').textContent = m.soc + ' %';
    document.getElementById('battBar').style.width = m.soc + '%';
    document.getElementById('battVolt').textContent = (m.voltage/1000).toFixed(2) + ' V';
    document.getElementById('battCurr').textContent = m.current + ' mA';
    document.getElementById('battTemp').textContent = m.temp.toFixed(1) + ' °C';
    document.getElementById('battCycle').textContent = m.cycles;
    document.getElementById('battCap').textContent = m.remain + '/' + m.full + ' mAh';
    document.getElementById('battDesign').textContent = m.design + ' mAh';
    document.getElementById('battMfr').textContent = m.mfr;
    document.getElementById('battDev').textContent = m.dev;
    document.getElementById('battChem').textContent = m.chem || '-';
    document.getElementById('battSN').textContent = m.sn;
    document.getElementById('battModel').textContent = m.model || '-';

    // Chip info
    const chipName = m.chipType === 0x4307 ? 'BQ40z307' :
                     m.chipType === 0x0550 ? 'BQ30z55' : 'unknown';
    document.getElementById('battChip').textContent = '0x' + m.chipType.toString(16).toUpperCase() + ' (' + chipName + ')';
    document.getElementById('battFwHw').textContent = 'FW:0x' + m.fwVer.toString(16) + ' HW:0x' + m.hwVer.toString(16);

    // Seal state
    const sealEl = document.getElementById('battSealStatus');
    if (m.sealed) {
      sealEl.style.display = 'inline-block';
      sealEl.textContent = 'SEALED';
      sealEl.className = 'status warn';
    } else {
      sealEl.style.display = 'inline-block';
      sealEl.textContent = 'UNSEALED';
      sealEl.className = 'status on';
    }

    // Key warning
    document.getElementById('keyWarning').style.display = m.needsKey ? 'block' : 'none';

    // Extended status (hex)
    const hex8 = v => '0x' + v.toString(16).toUpperCase().padStart(8, '0');
    document.getElementById('opStatus').textContent = hex8(m.operationStatus);
    document.getElementById('opDecoded').textContent = m.opDecoded;
    document.getElementById('safetyStatus').textContent = hex8(m.safetyStatus);
    document.getElementById('safetyDecoded').textContent = m.safetyDecoded;
    document.getElementById('pfStatus').textContent = hex8(m.pfStatus);
    const pfEl = document.getElementById('pfDecoded');
    pfEl.textContent = m.pfDecoded;
    pfEl.style.color = m.hasPF ? '#f44' : '#6f6';
    document.getElementById('mfgStatus').textContent = hex8(m.manufacturingStatus);
    document.getElementById('mfgDecoded').textContent = m.mfgDecoded;

    document.getElementById('cellsSync').textContent = m.cellsSync ? 'sync' : 'async';
    document.getElementById('packV').textContent = m.packV ? (m.packV/1000).toFixed(2) + ' V' : '-';

    let cellsHtml = '';
    const cells = m.cells || [];
    let minV = 9999, maxV = 0;
    cells.forEach((v, i) => {
      if (v > 0 && v < 5000) {
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
        const pct = Math.max(0, Math.min(100, (v-2500)/(4200-2500)*100));
        const col = v < 3300 ? '#f44' : v < 3600 ? '#fa0' : '#4f4';
        cellsHtml += `<div class="row"><span class="label">Cell ${i+1}</span><span class="value" style="color:${col}">${(v/1000).toFixed(3)} V</span></div>`;
        cellsHtml += `<div class="bar"><div class="bar-fill" style="width:${pct}%;background:${col}"></div></div>`;
      }
    });
    document.getElementById('cellsGrid').innerHTML = cellsHtml;
    const delta = (maxV > 0 && minV < 9999) ? (maxV - minV) : 0;
    const dEl = document.getElementById('cellDelta');
    dEl.textContent = delta + ' mV' + (delta > 50 ? ' UNBALANCED' : '');
    dEl.style.color = delta > 50 ? '#f44' : '#6f6';
  }
  else if (m.type === 'sys') {
    document.getElementById('sysIP').textContent = m.ip;
    document.getElementById('sysUptime').textContent = Math.floor(m.uptime/1000) + ' s';
    document.getElementById('sysHeap').textContent = (m.heap/1024).toFixed(1) + ' KB';
    document.getElementById('sysClients').textContent = m.clients;
  }
  else if (m.type === 'motor') {
    armed = m.armed;
    const s = document.getElementById('motorStatus');
    s.className = 'status ' + (armed ? 'on' : 'off');
    s.textContent = armed ? 'ARMED' : 'DISARMED';
    document.getElementById('throttleSlider').disabled = !armed;
    document.getElementById('armBtn').textContent = armed ? 'Re-arm' : 'ARM';
  }
  else if (m.type === 'crsf') {
    // Status badges
    const st = document.getElementById('crsfStatus');
    st.textContent = m.enabled ? 'ON' : 'OFF';
    st.className = 'status ' + (m.enabled ? 'on' : 'off');
    const co = document.getElementById('crsfConnected');
    co.textContent = m.connected ? 'LINK' : 'NO LINK';
    co.className = 'status ' + (m.connected ? 'on' : 'off');

    document.getElementById('crsfFrames').textContent = (m.frames || 0) + ' / ' + (m.badCrc || 0);

    // Link
    if (m.link) {
      document.getElementById('crsfRssi').textContent = m.link.rssi1 + ' dBm';
      document.getElementById('crsfLQ').textContent = m.link.lq + ' %';
      document.getElementById('crsfLqBar').style.width = m.link.lq + '%';
      document.getElementById('crsfSnr').textContent = m.link.snr + ' dB';
      document.getElementById('crsfRf').textContent = m.link.rf;
      document.getElementById('crsfPower').textContent = m.link.power + ' (idx)';
      document.getElementById('crsfDl').textContent = `RSSI ${m.link.dlRssi} dBm, LQ ${m.link.dlLq}%`;
    }

    // Channels
    if (m.ch) {
      let html = '';
      m.ch.forEach((v,i) => {
        // CRSF range 172-1811, center=992
        const pct = Math.max(0, Math.min(100, (v-172)/(1811-172)*100));
        const us = Math.round(((v - 992) * 5) / 8 + 1500); // convert to us approx
        html += `<div class="row"><span class="label">Ch ${i+1}</span><span class="value">${us}μs</span></div>`;
        html += `<div class="bar"><div class="bar-fill" style="width:${pct}%"></div></div>`;
      });
      document.getElementById('channelsGrid').innerHTML = html;
    }

    // FC telemetry
    if (m.mode) document.getElementById('crsfMode').textContent = m.mode;
    if (m.battery) {
      document.getElementById('crsfBatt').textContent =
        `${m.battery.v.toFixed(2)}V ${m.battery.i.toFixed(2)}A ${m.battery.mah}mAh (${m.battery.pct}%)`;
    }
    if (m.att) {
      document.getElementById('crsfAtt').textContent =
        `P:${m.att.p.toFixed(1)}° R:${m.att.r.toFixed(1)}° Y:${m.att.y.toFixed(1)}°`;
    }
    if (m.gps) {
      document.getElementById('crsfGPS').textContent =
        `${m.gps.lat.toFixed(6)}, ${m.gps.lon.toFixed(6)} alt=${m.gps.alt}m sats=${m.gps.sats}`;
    }

    // Device info
    if (m.device) {
      document.getElementById('crsfDevName').textContent = m.device.name;
      document.getElementById('crsfDevFw').textContent = '0x' + m.device.fw.toString(16);
      document.getElementById('crsfDevSerial').textContent = m.device.serial;
      document.getElementById('crsfDevFields').textContent = m.device.fields;
    }

    // Parameters (hierarchical by parent_id, state-aware COMMAND)
    if (m.params && m.params.length > 0) {
      document.getElementById('crsfParams').innerHTML = renderCrsfParams(m.params);
    }
  }
  else if (m.type === 'flash') {
    if (m.size > 0) {
      document.getElementById('fwSize').textContent = (m.size/1024).toFixed(1) + ' KB';
    }
    if (m.in_progress || m.progress > 0) {
      document.getElementById('flashStage').textContent = m.stage + ' (' + m.progress + '%)';
      document.getElementById('flashBar').style.width = m.progress + '%';
    }
    if (m.result && m.result !== '' && !m.in_progress) {
      document.getElementById('flashResult').textContent = 'Result: ' + m.result;
      document.getElementById('flashBtn').disabled = (m.size === 0);
    }
  }
}

connect();
</script>
</body>
</html>
)HTML";

const size_t WEB_INDEX_HTML_LEN = sizeof(WEB_INDEX_HTML) - 1;
