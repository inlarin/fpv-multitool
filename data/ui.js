let ws = null, wsOk = false;

// ELRS RX (ESP32-C3) partition layout — used by the slot-flash / OTADATA /
// wizard cards. Matches the Bayck RC C3 Dual dump analysed at
// hardware/bayckrc_c3_dual/.
const RX_PARTITIONS = {
  app0:    { offset: 0x10000,  size: 0x1e0000 },
  app1:    { offset: 0x1f0000, size: 0x1e0000 },
  otadata: { offset: 0xe000,   size: 0x2000   },
};

// Hardware.json blob extracted from app0 of the above dump — lets the user
// re-upload it to the ExpressLRS Configurator after a vanilla flash. No
// backend endpoint yet — served as a client-side download.
const RX_HARDWARE_JSON = {
  "serial_rx": 20, "serial_tx": 21,
  "radio_miso": 5, "radio_mosi": 4, "radio_sck": 6,
  "radio_busy": 3, "radio_dio1": 1,
  "radio_nss": 0, "radio_rst": 2,
  "radio_busy_2": 8, "radio_dio1_2": 18,
  "radio_nss_2": 7, "radio_rst_2": 10,
  "power_min": 0, "power_high": 3,
  "power_max": 3, "power_default": 0,
  "power_control": 0,
  "power_values": [12, 16, 19, 22],
  "power_values_dual": [-12, -9, -6, -2],
  "led_rgb": 19, "led_rgb_isgrb": true,
  "radio_dcdc": true,
  "button": 9,
  "radio_rfsw_ctrl": [31, 0, 4, 8, 8, 18, 0, 17]
};

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
  if (ws && wsOk) { ws.send(JSON.stringify(obj)); return; }
  // WS is down — POST to the HTTP fallback so commands aren't silently dropped.
  fetch('/api/ws', {method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(obj)
  }).catch(e => console.warn('send fallback failed:', e));
}

let _curWs = localStorage.getItem('ws') || 'batt';
const _wsDefTab = {batt:'battery', fpv:'servo', sys:'setup'};
const _wsValid = name => ['batt','fpv','sys'].includes(name);
const _tabValid = name => ['servo','motor','battery','receiver','rcsniff','setup','sys','usb','ota','_styleguide'].includes(name);
// Back-compat: any old saved 'elrs' or 'crsf' tab name maps to merged 'receiver' tab
if (localStorage.getItem('tab_fpv') === 'elrs' || localStorage.getItem('tab_fpv') === 'crsf') {
  localStorage.setItem('tab_fpv', 'receiver');
}

function showWorkspace(ws) {
  if (!_wsValid(ws)) ws = 'batt';
  _curWs = ws;
  localStorage.setItem('ws', ws);
  document.querySelectorAll('.ws-tab').forEach(e => e.classList.remove('active'));
  // Find and highlight the matching ws-tab (works for both click event and programmatic calls)
  const wsTabs = document.querySelectorAll('.ws-tab');
  const idx = ['batt','fpv','sys'].indexOf(ws);
  if (wsTabs[idx]) wsTabs[idx].classList.add('active');
  ['batt','fpv','sys'].forEach(w => {
    document.getElementById('ws-'+w).style.display = w === ws ? '' : 'none';
  });
  // Restore the last tab for this workspace, or fall back to default
  const savedTab = localStorage.getItem('tab_' + ws);
  const tabName = (savedTab && _tabValid(savedTab)) ? savedTab : _wsDefTab[ws];
  showTab(tabName);
}

// Per-tab poll timers — cleared on tab-leave so switching doesn't
// accumulate fetches forever (one per visited tab).
const _tabTimers = ['_escTelemTimer', '_smbLogTimer', '_vrvTimer',
                    'cpLogTimer', '_rcPollTimer', '_servoStateTimer',
                    '_dumpPoll', '_otaPullPollTimer',
                    '_slotFlashPoll', '_otadataPoll', '_flashPoll'];
function _clearTabTimers() {
  _tabTimers.forEach(name => {
    try {
      if (window[name]) { clearInterval(window[name]); window[name] = null; }
    } catch(e) {}
  });
}

function showTab(name) {
  if (!_tabValid(name)) name = _wsDefTab[_curWs];
  _clearTabTimers();  // stop any polling from the previous tab
  document.querySelectorAll('.tab-content').forEach(e => e.style.display = 'none');
  document.getElementById('tab-'+name).style.display = '';
  // Mark active tab within current workspace
  const wsEl = document.getElementById('ws-'+_curWs);
  if (wsEl) {
    wsEl.querySelectorAll('.tab').forEach(e => {
      e.classList.remove('active');
      if (e.getAttribute('onclick')?.includes("'"+name+"'")) e.classList.add('active');
    });
  }
  localStorage.setItem('tab_' + _curWs, name);
  if (name === 'ota') otaRefresh();
  if (name === 'servo') servoStatePoll();
  if (name === 'setup') setupRefresh();
  if (name === 'sys') portBRefresh();
  if (name === 'usb') { usbRefresh(); cpLogRefresh(); cpLogAutoToggle(); }
  if (name === 'battery') { loadProfiles(); loadMacCatalog(); showBattSub(_curBattSub, false); }
  if (name === 'receiver') {
    if (!window.ELRS_MODELS)   window.ELRS_MODELS = ELRS_MODELS;
    if (!window.ELRS_RELEASES) window.ELRS_RELEASES = ELRS_RELEASES;
    fwPopulateModels(); fwSourceChange(); fwPathUpdate();
    const tgt = document.getElementById('fwTarget'); if (tgt) tgt.onchange = fwPathUpdate;
    hwJsonPreviewRender();
    // Auto-probe on first open this session (or if stale >90s).
    const now = Date.now();
    if (!window._rxProbeLast || now - window._rxProbeLast > 90000) {
      window._rxProbeLast = now;
      setTimeout(() => { try { rxProbeMode(); } catch (_) {} }, 300);
    }
  }
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
  if (sweeping && !_servoStateTimer) _servoStateTimer = setInterval(servoStatePoll, 500);
}
function servoMark(which) { send({cmd:'servoMark'+which}); setTimeout(servoStatePoll, 100); }
function servoApplyMarks() { send({cmd:'servoApplyMarks'}); setTimeout(servoStatePoll, 100); }
function servoResetMarks() { send({cmd:'servoResetMarks'}); setTimeout(servoStatePoll, 100); }
function servoSweepCfgSave() {
  const minUs = +document.getElementById('sweepMinUs').value;
  const maxUs = +document.getElementById('sweepMaxUs').value;
  const periodMs = +document.getElementById('sweepPeriodMs').value;
  send({cmd:'servoSweepCfg', minUs, maxUs, periodMs});
  setTimeout(servoStatePoll, 100);
}
let _servoStateTimer = null;
function servoStatePoll() {
  fetch('/api/servo/state').then(r=>r.json()).then(d=>{
    document.getElementById('servoMarkMinVal').textContent = d.markedMinUs > 0 ? d.markedMinUs : '--';
    document.getElementById('servoMarkMaxVal').textContent = d.markedMaxUs > 0 ? d.markedMaxUs : '--';
    document.getElementById('sweepMinUs').value = d.sweepMinUs;
    document.getElementById('sweepMaxUs').value = d.sweepMaxUs;
    document.getElementById('sweepPeriodMs').value = d.sweepPeriodMs;
    const obs = (d.observedMinUs > 0 || d.observedMaxUs > 0)
      ? (d.observedMinUs + ' .. ' + d.observedMaxUs + ' us')
      : '--';
    document.getElementById('servoObserved').textContent = obs;
    if (sweeping !== d.sweep) {
      sweeping = d.sweep;
      document.getElementById('sweepBtn').textContent = sweeping ? 'Stop' : 'Start';
    }
  }).catch(()=>{});
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
function motorBeep(n) { send({cmd:'motorBeep', n: n || 1}); }
function onMaxThrottle() {
  const v = +document.getElementById('maxThrottleSlider').value;
  document.getElementById('maxThrottleVal').textContent = v;
  const slider = document.getElementById('throttleSlider');
  slider.max = v;
  if (+slider.value > v) { slider.value = v; onThrottle(); }
  send({cmd:'motorMaxThrottle', value: v});
}
async function motorDirCW()  { if (await confirmModal('Set direction CW (normal)? ESC must be armed.', {danger:true}))  send({cmd:'motorDirCW'}); }
async function motorDirCCW() { if (await confirmModal('Set direction CCW (reverse)? ESC must be armed.', {danger:true})) send({cmd:'motorDirCCW'}); }
async function motor3DOn()   { if (await confirmModal('Enable 3D mode? ESC must be armed.', {danger:true}))  send({cmd:'motor3DOn'}); }
async function motor3DOff()  { if (await confirmModal('Disable 3D mode? ESC must be armed.', {danger:true})) send({cmd:'motor3DOff'}); }

// === ESC telemetry ===
let _escTelemTimer = null;
function escTelemStart() {
  const poles = +document.getElementById('escTelemPoles').value || 14;
  const fd = new FormData(); fd.append('poles', poles);
  fetch('/api/esc/telem/start', {method:'POST', body: fd}).then(r=>r.text()).then(t=>{
    if (!_escTelemTimer) _escTelemTimer = setInterval(escTelemPoll, 500);
    escTelemPoll();
  });
}
function escTelemStop() {
  fetch('/api/esc/telem/stop', {method:'POST'}).then(()=>{
    if (_escTelemTimer) { clearInterval(_escTelemTimer); _escTelemTimer = null; }
    escTelemPoll();
  });
}
function escTelemPoll() {
  fetch('/api/esc/telem/state').then(r=>r.json()).then(d=>{
    const conn = document.getElementById('escTelemConn');
    if (!d.running) { conn.textContent='OFF'; conn.className='status off'; }
    else if (d.connected) { conn.textContent='CONNECTED'; conn.className='status on'; }
    else { conn.textContent='NO DATA'; conn.className='status off'; }
    document.getElementById('escTelemRate').textContent =
      d.frameRateHz + ' Hz (pp=' + d.polePairs + ')';
    if (d.frameCount > 0) {
      document.getElementById('escTelemTV').textContent =
        d.temp_c + '\u00B0C / ' + d.voltage_V.toFixed(2) + ' V';
      document.getElementById('escTelemCI').textContent =
        d.current_A.toFixed(2) + ' A / ' + d.consumption_mAh + ' mAh';
      document.getElementById('escTelemRPM').textContent =
        d.erpm + ' / ' + d.rpm;
    }
    document.getElementById('escTelemMax').textContent =
      d.maxTemp + '\u00B0C | ' + d.maxCurrent_A.toFixed(2) + ' A | ' +
      d.minVoltage_V.toFixed(2) + ' V | ' + d.peakVoltage_V.toFixed(2) + ' V | ' +
      d.maxErpm + ' eRPM';
    document.getElementById('escTelemStats').textContent =
      d.frameCount + ' / ' + d.crcErrors;
  }).catch(()=>{});
}

// === BATTERY ===
async function battAction(action) {
  const confirmMsgs = {
    unseal:      'Unseal battery? Tries TI default key.\nMavic 2+/3/4 will likely fail (no public key).',
    clearpf:     'Clear Permanent Failure flags?\nBattery MUST be unsealed first.\nOperation can take 2-3 seconds.',
    seal:        'Seal battery (lock config)?',
    reset:       'Send soft reset to BMS?',
    fullservice: 'FULL SERVICE: unseal + clear PF + seal.\nPotentially destructive. Continue only if you know what you are doing.'
  };
  if (confirmMsgs[action] && !(await confirmModal(confirmMsgs[action], {danger: action === 'fullservice' || action === 'clearpf'}))) return;
  document.getElementById('battActionResult').textContent = '...';
  fetch('/api/batt?action='+action).then(r=>r.text()).then(t=>{
    document.getElementById('battActionResult').textContent = t;
  });
}

// === BATTERY LAB ===
let _profiles = [], _macCatalog = [];

function loadProfiles() {
  fetch('/api/batt/profiles').then(r=>r.json()).then(p=>{
    _profiles = p;
    const sel = document.getElementById('unsealProfile');
    sel.innerHTML = '';
    p.forEach((pr,i) => { const o = document.createElement('option'); o.value=i; o.textContent=pr.name; sel.appendChild(o); });
    loadProfile();
  });
}
function loadProfile() {
  const p = _profiles[document.getElementById('unsealProfile').value];
  if (!p) return;
  const el = document.getElementById('unsealKeyList');
  el.innerHTML = '<b>Unseal:</b> ' + p.unsealKeys.map(k=>k.w1+'/'+k.w2+' ('+k.desc+')').join(', ')
    + '<br><b>FAS:</b> ' + p.fasKeys.map(k=>k.w1+'/'+k.w2+' ('+k.desc+')').join(', ');
}
function tryAllKeys() {
  const p = _profiles[document.getElementById('unsealProfile').value];
  if (!p) return;
  const el = document.getElementById('unsealResult');
  el.textContent = 'Trying...';
  let chain = Promise.resolve();
  p.unsealKeys.forEach(k => {
    chain = chain.then(() => fetch('/api/batt/diag?unseal='+k.w1+','+k.w2).then(r=>r.json()).then(j=>{
      el.textContent += '\n' + k.desc + ': ' + j.result + (j.sealed ? ' (still sealed)' : ' UNSEALED!');
      if (!j.sealed) throw 'done';
    }));
  });
  chain.catch(e=>{ if (e!=='done') el.textContent += '\nError: '+e; });
}
function tryFasKeys() {
  const p = _profiles[document.getElementById('unsealProfile').value];
  if (!p) return;
  const el = document.getElementById('unsealResult');
  el.textContent = 'Trying FAS...';
  let chain = Promise.resolve();
  p.fasKeys.forEach(k => {
    chain = chain.then(() => fetch('/api/batt/diag?unseal='+k.w1+','+k.w2).then(r=>r.json()).then(j=>{
      el.textContent += '\n' + k.desc + ': ' + j.result;
    }));
  });
  chain.catch(e=>{ el.textContent += '\nError: '+e; });
}
function tryManualKey() {
  const w1 = document.getElementById('unsealW1').value || '0';
  const w2 = document.getElementById('unsealW2').value || '0';
  const el = document.getElementById('unsealResult');
  el.textContent = 'Sending ' + w1 + '/' + w2 + '...';
  fetch('/api/batt/diag?unseal='+w1+','+w2).then(r=>r.json()).then(j=>{
    el.textContent = j.result + ' | sealed=' + j.sealed + ' | opStatus=' + j.opStatus;
  });
}

// === CLONE EXPLORER ===
function _ceLog(line) {
  const el = document.getElementById('ceResult');
  el.textContent += line + '\n';
  el.scrollTop = el.scrollHeight;
}
function _ceClear() { document.getElementById('ceResult').textContent = ''; }

function ceSbsScan() {
  _ceClear();
  _ceLog('Scanning SBS 0x00-0xFF...');
  document.getElementById('ceStatus').textContent = 'scanning SBS...';
  fetch('/api/batt/scan/sbs?from=0x00&to=0xFF').then(r=>r.json()).then(entries=>{
    document.getElementById('ceStatus').textContent = entries.length + ' responsive registers';
    _ceLog('');
    _ceLog('reg  | word/dec       | len | hex                                  | ascii');
    _ceLog('-----+----------------+-----+--------------------------------------+-----------------');
    entries.forEach(e => {
      const wDec = e.dec !== undefined ? String(e.dec).padEnd(5) : '     ';
      const w = e.word ? e.word + ' (' + wDec + ')' : '              ';
      const bhex = (e.bhex || '').padEnd(32);
      const bascii = (e.bascii || '').padEnd(16);
      _ceLog(e.reg.padEnd(5) + '| ' + w.padEnd(15) + '| ' + String(e.blen ?? '').padEnd(4) + '| ' + bhex + '| ' + bascii);
    });
  }).catch(err => _ceLog('Error: ' + err));
}

function ceMacScan(from, to) {
  _ceClear();
  _ceLog('Scanning MAC 0x' + from.toString(16).padStart(4,'0').toUpperCase() + '-0x' + to.toString(16).padStart(4,'0').toUpperCase() + '...');
  _ceLog('(this takes ~30s per 256 subcommands due to I2C delay)');
  document.getElementById('ceStatus').textContent = 'scanning MAC...';
  fetch('/api/batt/scan/mac?from=0x' + from.toString(16) + '&to=0x' + to.toString(16)).then(r=>r.json()).then(entries=>{
    document.getElementById('ceStatus').textContent = entries.length + ' MAC subcommands returned data';
    _ceLog('');
    if (!entries.length) {
      _ceLog('(no MAC subcommand returned non-zero data)');
      return;
    }
    _ceLog('sub     | len | hex                              | ascii');
    _ceLog('--------+-----+----------------------------------+-----------------');
    entries.forEach(e => {
      _ceLog(e.sub.padEnd(8) + '| ' + String(e.len).padEnd(4) + '| ' + e.hex.padEnd(32) + ' | ' + e.ascii);
    });
  }).catch(err => _ceLog('Error: ' + err));
}

function ceBypassTest() {
  _ceClear();
  _ceLog('Seal bypass test — probes if seal protection is actually enforced.');
  _ceLog('');
  // Safe: write current cycle count value back to itself. If it "persists", seal is cosmetic.
  // Plus: try DF write 0x4340 (cycle count in DF) with zero, read back.
  _ceLog('Step 1: read current cycleCount (SBS 0x17) ...');
  const ceDo = async () => {
    const cur = await fetch('/api/batt/snapshot').then(r=>r.json());
    const cyc = cur.cycleCount;
    _ceLog('  current: ' + cyc);
    _ceLog('');

    _ceLog('Step 2: try writeWord to SBS 0x17 (sealed regs normally reject)...');
    const fd = new FormData();
    fd.append('reg', '0x17');
    fd.append('value', '65535');  // deliberately silly value
    const r = await fetch('/api/batt/scan/wv', {method:'POST', body:fd}).then(r=>r.json());
    _ceLog('  before=' + r.before + ' target=' + r.target + ' after=' + r.after + ' wrote=' + r.wrote);
    if (r.seal_bypassed) {
      _ceLog('  ⚠ SEAL BYPASSED! write persisted → clone does not enforce seal on SBS 0x17');
    } else if (r.wrote && !r.persisted) {
      _ceLog('  ✓ seal enforced (write accepted but value ignored) — normal behaviour');
    } else if (!r.wrote) {
      _ceLog('  ✓ seal enforced (write rejected at I2C level)');
    }
    _ceLog('');

    _ceLog('Step 3: try DF 0x4340 (cycle count in DF) write via MAC 0x44...');
    const fd2 = new FormData();
    fd2.append('addr', '0x4340'); fd2.append('value', '0'); fd2.append('size', '2');
    const r2 = await fetch('/api/batt/df/write', {method:'POST', body:fd2}).then(r=>r.json());
    _ceLog('  DF write ok=' + r2.ok);
    await new Promise(s => setTimeout(s, 300));
    const cur2 = await fetch('/api/batt/snapshot').then(r=>r.json());
    _ceLog('  cycleCount after: ' + cur2.cycleCount);
    if (cur2.cycleCount === 0 && cyc > 0) {
      _ceLog('  ⚠ CYCLE COUNT WIPED! DF write to 0x4340 worked without unseal!');
    } else if (cur2.cycleCount === cyc) {
      _ceLog('  ✓ DF protected (cycleCount unchanged)');
    }
    _ceLog('');
    _ceLog('Done. Also try "Scan MAC 0x0000-0x00FF" to find vendor-specific commands.');
    document.getElementById('ceStatus').textContent = 'bypass test complete';
  };
  ceDo().catch(e => _ceLog('Error: ' + e));
}

// === FLEET COMPARE ===
let _fleet = [];
function fleetAdd(evt) {
  const files = Array.from(evt.target.files || []);
  let done = 0;
  files.forEach(f => {
    const r = new FileReader();
    r.onload = (e) => {
      try {
        const snap = JSON.parse(e.target.result);
        _fleet.push({name: f.name, data: snap});
      } catch (err) {
        alert('Bad JSON in ' + f.name + ': ' + err);
      }
      if (++done === files.length) fleetRender();
    };
    r.readAsText(f);
  });
  evt.target.value = '';
}
function fleetAddCurrent() {
  fetch('/api/batt/snapshot').then(r=>r.json()).then(snap=>{
    const name = (snap.mfrName || 'batt') + '-' + (snap.serialNumber || '?') + ' @ ' + new Date().toISOString().slice(11,19);
    _fleet.push({name, data: snap});
    fleetRender();
  });
}
function fleetClear() { _fleet = []; fleetRender(); }

function _fleetColor(val, good, warn) {
  if (val == null || val === undefined) return 'var(--text-muted)';
  if (val >= good) return 'var(--status-on)';
  if (val >= warn) return 'var(--status-warn)';
  return 'var(--warning-text)';
}

function fleetRender() {
  const el = document.getElementById('fleetTable');
  const status = document.getElementById('fleetStatus');
  status.textContent = _fleet.length + ' snapshot' + (_fleet.length === 1 ? '' : 's');
  if (!_fleet.length) { el.innerHTML = '<div style="color:var(--text-dim);padding:10px">No snapshots loaded.</div>'; return; }

  // Compute cell deltas for each
  const row = (s) => {
    const cells = s.cellVoltageSync_mV || s.cellVoltage_mV || [];
    const nonZero = cells.filter(v => v > 100 && v < 5000);
    const minC = nonZero.length ? Math.min(...nonZero) : 0;
    const maxC = nonZero.length ? Math.max(...nonZero) : 0;
    const delta = maxC - minC;
    return {
      name: s.deviceName || '-',
      mfr: s.mfrName || '-',
      model: s.model || '-',
      v: (s.voltage_mV / 1000).toFixed(2),
      soc: s.soc || 0,
      soh: s.stateOfHealth || null,
      cycles: s.cycleCount || 0,
      cap: s.fullCap_mAh || 0,
      design: s.designCap_mAh || 0,
      wear: s.designCap_mAh ? Math.round((1 - (s.fullCap_mAh / s.designCap_mAh)) * 100) : null,
      delta: delta,
      cells: nonZero.length,
      temp: (s.temperature_C || 0).toFixed(1),
      sealed: s.sealed ? 'SEALED' : 'open',
      pf: s.hasPF,
    };
  };

  let html = '<table style="width:100%;border-collapse:collapse;font-size:11px;font-family:monospace">';
  html += '<thead><tr style="background:var(--card-bg2);color:var(--accent)">';
  html += '<th style="padding:4px;text-align:left">Snapshot</th>';
  html += '<th style="padding:4px">Model</th>';
  html += '<th style="padding:4px">V</th>';
  html += '<th style="padding:4px">SOC%</th>';
  html += '<th style="padding:4px">SOH%</th>';
  html += '<th style="padding:4px">Cycles</th>';
  html += '<th style="padding:4px">Full/Design</th>';
  html += '<th style="padding:4px">Wear%</th>';
  html += '<th style="padding:4px">Δcell</th>';
  html += '<th style="padding:4px">Cells</th>';
  html += '<th style="padding:4px">T°C</th>';
  html += '<th style="padding:4px">State</th>';
  html += '</tr></thead><tbody>';

  _fleet.forEach((item, i) => {
    const r = row(item.data);
    html += '<tr style="border-bottom:1px solid var(--border-soft)">';
    html += '<td style="padding:3px 4px;color:var(--text)" title="' + (item.name||'') + '">'
          + (item.name || '').slice(0, 30) + '</td>';
    html += '<td style="padding:3px 4px">' + r.model + '</td>';
    html += '<td style="padding:3px 4px;text-align:right">' + r.v + '</td>';
    html += '<td style="padding:3px 4px;text-align:right;color:' + _fleetColor(r.soc, 50, 20) + '">' + r.soc + '</td>';
    html += '<td style="padding:3px 4px;text-align:right;color:' + _fleetColor(r.soh, 80, 60) + '">' + (r.soh ?? '-') + '</td>';
    html += '<td style="padding:3px 4px;text-align:right;color:' + _fleetColor(200 - r.cycles, 100, 50) + '">' + r.cycles + '</td>';
    html += '<td style="padding:3px 4px;text-align:right">' + r.cap + '/' + r.design + '</td>';
    const wearCol = r.wear == null ? 'var(--text-muted)' : (r.wear < 10 ? 'var(--status-on)' : r.wear < 25 ? 'var(--status-warn)' : 'var(--warning-text)');
    html += '<td style="padding:3px 4px;text-align:right;color:' + wearCol + '">' + (r.wear ?? '-') + '</td>';
    const deltaCol = r.delta < 30 ? 'var(--status-on)' : r.delta < 80 ? 'var(--status-warn)' : 'var(--warning-text)';
    html += '<td style="padding:3px 4px;text-align:right;color:' + deltaCol + '">' + r.delta + '</td>';
    html += '<td style="padding:3px 4px;text-align:right">' + r.cells + 'S</td>';
    html += '<td style="padding:3px 4px;text-align:right">' + r.temp + '</td>';
    html += '<td style="padding:3px 4px;color:' + (r.pf ? 'var(--warning-text)' : (r.sealed === 'SEALED' ? 'var(--status-warn)' : 'var(--status-on)')) + '">';
    html += r.pf ? 'PF!' : r.sealed;
    html += '</td></tr>';
  });
  html += '</tbody></table>';

  // Summary row: averages & outlier detection
  if (_fleet.length >= 2) {
    const rows = _fleet.map(x => row(x.data));
    const avg = (f) => (rows.reduce((s,r) => s + (r[f]||0), 0) / rows.length).toFixed(1);
    html += '<div style="margin-top:6px;font-size:11px;color:var(--text-dim)">';
    html += 'Averages: SOC ' + avg('soc') + '%, SOH ' + avg('soh') + '%, Cycles ' + avg('cycles')
         + ', Wear ' + avg('wear') + '%, Δcell ' + avg('delta') + 'mV';
    html += '</div>';
  }

  el.innerHTML = html;
}

function tryHmacUnseal() {
  const key = document.getElementById('unsealHmacKey').value.trim().replace(/[^0-9a-fA-F]/g, '');
  if (key.length !== 64) { alert('Need 64 hex chars (32 bytes), got ' + key.length); return; }
  const el = document.getElementById('hmacResult');
  el.style.display = 'block';
  el.textContent = 'Sending HMAC unseal...';
  const fd = new FormData(); fd.append('key', key);
  fetch('/api/batt/unseal_hmac', {method:'POST', body:fd}).then(r=>r.json()).then(j=>{
    el.textContent = 'Result: ' + j.result + '\nChallenge: ' + (j.challenge || '(none)');
    el.style.color = j.ok ? 'var(--status-on)' : 'var(--warning-text)';
  }).catch(e=>{ el.textContent = 'Error: ' + e; el.style.color = 'var(--warning-text)'; });
}

function tryGetChallenge() {
  // Read 20 bytes from MAC 0x0000 via /api/batt/diag?mac=0x0000 — no key needed
  const el = document.getElementById('hmacResult');
  el.style.display = 'block';
  el.textContent = 'Reading MAC 0x0000 challenge...';
  fetch('/api/batt/diag?mac=0x0000').then(r=>r.json()).then(j=>{
    el.textContent = 'Challenge (len=' + j.len + '):\n' + j.hex + '\nASCII: ' + j.ascii;
    el.style.color = 'var(--accent)';
  });
}

function loadMacCatalog() {
  fetch('/api/batt/mac_catalog').then(r=>r.json()).then(c=>{
    _macCatalog = c;
    const sel = document.getElementById('macSelect');
    sel.innerHTML = '';
    c.forEach(m => {
      const o = document.createElement('option');
      o.value = m.sub; o.textContent = m.sub + ' ' + m.name + (m.destructive ? ' [!]' : '');
      o.dataset.desc = m.desc; o.dataset.rlen = m.rlen; o.dataset.destructive = m.destructive;
      sel.appendChild(o);
    });
    sel.onchange = () => {
      const opt = sel.selectedOptions[0];
      document.getElementById('macDesc').textContent = opt ? opt.dataset.desc : '';
    };
    sel.onchange();
  });
}
async function runMacCmd() {
  const sel = document.getElementById('macSelect');
  const opt = sel.selectedOptions[0];
  if (!opt) return;
  if (opt.dataset.destructive === 'true' && !(await confirmModal(opt.dataset.desc + '\nThis is destructive. Continue?', {danger:true}))) return;
  const sub = opt.value;
  const rlen = parseInt(opt.dataset.rlen);
  const el = document.getElementById('macResult');
  el.textContent = 'Executing ' + sub + '...';
  if (rlen > 0) {
    fetch('/api/batt/diag?mac=' + sub).then(r=>r.json()).then(j=>{
      el.textContent = 'MAC ' + sub + ' => len=' + j.len + '\nhex: ' + j.hex + '\nascii: ' + j.ascii;
    });
  } else {
    fetch('/api/smbus/xact?addr=0x0B&op=macCmd&data=' + sub).then(r=>r.json()).then(j=>{
      el.textContent = 'MAC ' + sub + ' => ' + (j.ok ? 'OK' : 'FAIL');
    });
  }
}

function smbExec() {
  const addr = document.getElementById('smbAddr').value;
  const op = document.getElementById('smbOp').value;
  const reg = document.getElementById('smbReg').value;
  const data = document.getElementById('smbData').value;
  const el = document.getElementById('smbResult');
  el.textContent = 'Sending...';
  let url = '/api/smbus/xact?addr='+addr+'&op='+op+'&reg='+reg;
  if (data) url += '&data='+encodeURIComponent(data);
  fetch(url).then(r=>r.json()).then(j=>{
    el.textContent = JSON.stringify(j, null, 2);
  });
}
// === DATA FLASH EDITOR ===
let _dfMap = [], _dfValues = {}, _dfSnapA = null, _dfSnapB = null;

function dfLoadMap() {
  // If we have a custom map imported from Killer.ini, prefer that
  const custom = localStorage.getItem('dfMapCustom');
  if (custom) {
    try {
      _dfMap = JSON.parse(custom);
      document.getElementById('dfReadAllBtn').disabled = false;
      document.getElementById('dfFilter').style.display = '';
      document.getElementById('dfStatus').textContent = _dfMap.length + ' entries (custom Killer.ini)';
      dfRenderTree();
      return;
    } catch (e) { localStorage.removeItem('dfMapCustom'); }
  }
  document.getElementById('dfStatus').textContent = 'Loading built-in map...';
  fetch('/api/batt/df/map').then(r=>r.json()).then(m=>{
    _dfMap = m;
    document.getElementById('dfReadAllBtn').disabled = false;
    document.getElementById('dfFilter').style.display = '';
    document.getElementById('dfStatus').textContent = m.length + ' entries (built-in)';
    dfRenderTree();
  });
}

// Parse Killer.ini content — format per line:
//   "Category", "SubCategory", "Field", Address, Type, Min, Max, Default, "Unit"
// Lines starting with ; or [ are skipped; NULL addresses are skipped.
function dfParseKillerIni(text) {
  const out = [];
  // Split fields on commas that are NOT inside quotes
  const splitCSV = (line) => {
    const parts = []; let buf = ''; let inQuote = false;
    for (const c of line) {
      if (c === '"') { inQuote = !inQuote; continue; }
      if (c === ',' && !inQuote) { parts.push(buf.trim()); buf = ''; continue; }
      buf += c;
    }
    if (buf.length) parts.push(buf.trim());
    return parts;
  };
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith(';') || line.startsWith('[')) continue;
    const p = splitCSV(line);
    if (p.length < 9) continue;
    const [cat, sub, field, addrRaw, typeRaw, minRaw, maxRaw, defRaw, unit] = p;
    if (addrRaw === 'NULL' || addrRaw === 'null' || addrRaw === '') continue;
    const addrNum = parseInt(addrRaw, addrRaw.startsWith('0x') ? 16 : 10);
    if (isNaN(addrNum)) continue;
    const type = typeRaw.trim();
    const typeSize = {I1:1, U1:1, H1:1, I2:2, U2:2, U4:4}[type];
    if (!typeSize) continue;
    const parseNum = s => {
      s = s.trim();
      return s.startsWith('0x') ? parseInt(s, 16) : parseInt(s, 10);
    };
    out.push({
      addr: '0x' + addrNum.toString(16).toUpperCase(),
      type, size: typeSize,
      min: parseNum(minRaw),
      max: parseNum(maxRaw),
      def: parseNum(defRaw),
      cat, sub, field, unit
    });
  }
  return out;
}

function dfImportKillerIni(evt) {
  const file = evt.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = async (e) => {
    try {
      const parsed = dfParseKillerIni(e.target.result);
      if (parsed.length < 10) {
        if (!(await confirmModal('Only ' + parsed.length + ' entries parsed. Continue anyway?'))) return;
      }
      localStorage.setItem('dfMapCustom', JSON.stringify(parsed));
      _dfMap = parsed;
      _dfValues = {};
      document.getElementById('dfReadAllBtn').disabled = false;
      document.getElementById('dfFilter').style.display = '';
      document.getElementById('dfStatus').innerHTML = parsed.length + ' entries <b style="color:var(--accent)">(from ' + file.name + ')</b> <a href="#" onclick="dfClearCustomMap(); return false;" style="color:var(--warning-text)">[reset to built-in]</a>';
      dfRenderTree();
    } catch (err) {
      alert('Parse failed: ' + err);
    }
  };
  reader.readAsText(file);
  evt.target.value = '';
}

async function dfClearCustomMap() {
  if (!(await confirmModal('Discard imported Killer.ini and use built-in map?'))) return;
  localStorage.removeItem('dfMapCustom');
  dfLoadMap();
}

function dfReadAll() {
  const btn = document.getElementById('dfReadAllBtn');
  btn.disabled = true;
  document.getElementById('dfStatus').textContent = 'Reading all DF values...';
  fetch('/api/batt/df/readall').then(r=>r.json()).then(vals=>{
    _dfValues = {};
    vals.forEach(v => { _dfValues[v.addr] = v; });
    btn.disabled = false;
    document.getElementById('dfExportBtn').disabled = false;
    let ok = 0, fail = 0, diff = 0;
    vals.forEach(v => { if (v.ok) { ok++; if (v.diff) diff++; } else fail++; });
    document.getElementById('dfStatus').textContent = 'Read: ' + ok + ' ok, ' + fail + ' fail, ' + diff + ' differ from default';
    dfRenderTree();
  }).catch(e => {
    btn.disabled = false;
    document.getElementById('dfStatus').textContent = 'Error: ' + e;
  });
}

function _dfSnapVal(snap, addr) {
  if (!snap) return null;
  const row = snap.find(r => r.addr === addr);
  if (!row || row.current === null || row.current === undefined) return null;
  return row.current;
}

function dfRenderTree() {
  const search = (document.getElementById('dfSearch')?.value || '').toLowerCase();
  const diffOnly = document.getElementById('dfDiffOnly')?.checked;
  const compareMode = document.getElementById('dfCompareMode')?.checked;
  const hasA = !!_dfSnapA, hasB = !!_dfSnapB;

  // Group by category -> subcategory
  const groups = {};
  _dfMap.forEach((e, idx) => {
    const v = _dfValues[e.addr];
    const a = hasA ? _dfSnapVal(_dfSnapA, e.addr) : null;
    const b = hasB ? _dfSnapVal(_dfSnapB, e.addr) : null;
    const fullName = (e.cat + ' ' + e.sub + ' ' + e.field).toLowerCase();
    if (search && !fullName.includes(search)) return;
    // In compare mode: only show rows where A and B differ
    if (compareMode && hasA && hasB && a === b) return;
    if (diffOnly && !compareMode && (!v || !v.diff)) return;
    const key = e.cat;
    if (!groups[key]) groups[key] = {};
    if (!groups[key][e.sub]) groups[key][e.sub] = [];
    groups[key][e.sub].push({...e, idx, v, a, b});
  });

  let html = '';
  if (compareMode && (hasA || hasB)) {
    html += '<div style="color:#888;font-size:11px;margin:4px 0">';
    html += '<span style="color:#0af">A</span> = imported snapshot A, ';
    html += '<span style="color:#f0a">B</span> = imported snapshot B. ';
    html += 'Yellow = differs.';
    html += '</div>';
  }
  for (const cat in groups) {
    html += '<div style="margin-top:8px;border-left:3px solid #55a;padding-left:8px">';
    html += '<div style="color:#8af;font-weight:bold;margin-bottom:4px">' + cat + '</div>';
    for (const sub in groups[cat]) {
      html += '<div style="color:#aaa;margin:4px 0 2px 8px;font-size:11px">▸ ' + sub + '</div>';
      html += '<table style="width:100%;border-collapse:collapse;margin-left:16px">';
      groups[cat][sub].forEach(e => {
        const v = e.v;
        const hasVal = v && v.ok;
        const curVal = hasVal ? v.val : '';
        const isDiff = hasVal && v.diff;
        const valColor = isDiff ? '#fa0' : '#6f6';
        const addrHex = e.addr;
        html += '<tr style="border-bottom:1px solid #1a1a2e">';
        html += '<td style="padding:3px 6px;color:#ccc;width:30%">' + e.field + ' <span style="color:#555">(' + e.unit + ')</span></td>';
        html += '<td style="padding:3px 4px;color:#666;font-family:monospace;font-size:10px;width:60px">' + addrHex + '</td>';
        if (compareMode) {
          const diffAB = (e.a !== null && e.b !== null && e.a !== e.b);
          const colA = diffAB ? '#fa0' : '#0af';
          const colB = diffAB ? '#fa0' : '#f0a';
          html += '<td style="padding:3px 4px;color:' + colA + ';font-family:monospace;font-size:12px;width:80px">'
                + (e.a !== null ? e.a : '<span style="color:#444">-</span>') + '</td>';
          html += '<td style="padding:3px 4px;color:' + colB + ';font-family:monospace;font-size:12px;width:80px">'
                + (e.b !== null ? e.b : '<span style="color:#444">-</span>') + '</td>';
          html += '<td style="padding:3px 4px;color:#555;font-size:11px;width:60px">';
          if (diffAB) {
            const delta = e.b - e.a;
            html += (delta > 0 ? '+' : '') + delta;
          }
          html += '</td>';
          // Restore button — write A value (if available) to board
          html += '<td style="padding:3px 2px;width:60px">';
          if (hasVal && e.a !== null && e.a !== curVal) {
            html += '<button onclick="dfRestoreOne(this)" data-addr="' + addrHex + '" data-size="' + e.size + '" data-value="' + e.a + '" '
                  + 'style="padding:2px 6px;font-size:10px;background:#08a">→A</button>';
          }
          if (hasVal && e.b !== null && e.b !== curVal) {
            html += ' <button onclick="dfRestoreOne(this)" data-addr="' + addrHex + '" data-size="' + e.size + '" data-value="' + e.b + '" '
                  + 'style="padding:2px 6px;font-size:10px;background:#a08">→B</button>';
          }
          html += '</td></tr>';
        } else {
          html += '<td style="padding:3px 4px;width:30%">';
          if (hasVal) {
            html += '<input type="number" value="' + curVal + '" min="' + e.min + '" max="' + e.max + '" '
                  + 'data-addr="' + addrHex + '" data-size="' + e.size + '" data-orig="' + curVal + '" '
                  + 'style="width:80px;background:#0a0a14;color:' + valColor + ';border:1px solid #333;padding:2px 4px;font-family:monospace;font-size:12px" '
                  + 'onchange="dfMarkChanged(this)">';
          } else {
            html += '<span style="color:#555">-</span>';
          }
          html += '</td>';
          html += '<td style="padding:3px 4px;width:60px">';
          if (hasVal) html += '<span style="color:#555;font-size:10px">def:' + e.def + '</span>';
          html += '</td>';
          html += '<td style="padding:3px 2px;width:50px">';
          if (hasVal) {
            html += '<button onclick="dfWriteOne(this)" data-addr="' + addrHex + '" data-size="' + e.size + '" '
                  + 'style="padding:2px 6px;font-size:10px;background:#333;display:none">Write</button>';
          }
          html += '</td></tr>';
        }
      });
      html += '</table>';
    }
    html += '</div>';
  }
  if (!html) html = '<div style="color:#888;padding:10px">No entries. Click "Load Map" first.</div>';
  document.getElementById('dfTree').innerHTML = html;
}

function dfMarkChanged(input) {
  const orig = parseInt(input.dataset.orig);
  const cur = parseInt(input.value);
  const writeBtn = input.closest('tr').querySelector('button');
  if (cur !== orig) {
    input.style.color = '#f44';
    input.style.borderColor = '#f44';
    if (writeBtn) writeBtn.style.display = '';
  } else {
    input.style.color = '#6f6';
    input.style.borderColor = '#333';
    if (writeBtn) writeBtn.style.display = 'none';
  }
}

async function dfWriteOne(btn) {
  const addr = btn.dataset.addr;
  const size = btn.dataset.size;
  const input = btn.closest('tr').querySelector('input[type=number]');
  const value = input.value;
  if (!(await confirmModal('Write ' + value + ' to DF ' + addr + '?\nBattery must be unsealed!', {danger:true}))) return;
  btn.disabled = true;
  btn.textContent = '...';
  const fd = new FormData();
  fd.append('addr', addr); fd.append('value', value); fd.append('size', size);
  fetch('/api/batt/df/write', {method:'POST', body:fd}).then(r=>r.json()).then(j=>{
    btn.disabled = false;
    if (j.ok) {
      btn.textContent = 'OK';
      btn.style.background = '#2a4';
      input.dataset.orig = value;
      input.style.color = '#6f6';
      input.style.borderColor = '#333';
      setTimeout(()=>{ btn.textContent = 'Write'; btn.style.background = '#333'; btn.style.display = 'none'; }, 1500);
    } else {
      btn.textContent = 'FAIL';
      btn.style.background = '#a42';
      setTimeout(()=>{ btn.textContent = 'Write'; btn.style.background = '#333'; }, 2000);
    }
  });
}

function dfFilterApply() { dfRenderTree(); }

function _dfSnapshotFromCurrent() {
  return _dfMap.map(e => {
    const v = _dfValues[e.addr];
    return {
      addr: e.addr, category: e.cat, subcat: e.sub, field: e.field,
      type: e.type, unit: e.unit, default: e.def,
      current: v && v.ok ? v.val : null,
      differs: v ? !!v.diff : null
    };
  });
}

function dfExport() {
  const blob = new Blob([JSON.stringify(_dfSnapshotFromCurrent(), null, 2)], {type:'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'dataflash_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.json';
  a.click();
}

function dfImportSnapshot(evt, slot) {
  const file = evt.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = (e) => {
    try {
      const data = JSON.parse(e.target.result);
      if (!Array.isArray(data)) throw new Error('Expected array');
      if (slot === 'A') _dfSnapA = data; else _dfSnapB = data;
      dfUpdateSnapshotStatus();
      // Auto-enable compare mode when both loaded
      if (_dfSnapA && _dfSnapB) document.getElementById('dfCompareMode').checked = true;
      dfRenderTree();
    } catch (err) {
      alert('Bad snapshot file: ' + err);
    }
  };
  reader.readAsText(file);
  evt.target.value = '';
}

function dfUseCurrentAs(slot) {
  if (!Object.keys(_dfValues).length) { alert('Read all values first'); return; }
  const snap = _dfSnapshotFromCurrent();
  if (slot === 'A') _dfSnapA = snap; else _dfSnapB = snap;
  dfUpdateSnapshotStatus();
  if (_dfSnapA && _dfSnapB) document.getElementById('dfCompareMode').checked = true;
  dfRenderTree();
}

function dfClearSnapshots() {
  _dfSnapA = null; _dfSnapB = null;
  document.getElementById('dfCompareMode').checked = false;
  dfUpdateSnapshotStatus();
  dfRenderTree();
}

function dfUpdateSnapshotStatus() {
  const el = document.getElementById('dfSnapshotStatus');
  const parts = [];
  if (_dfSnapA) parts.push('<span style="color:#0af">A loaded (' + _dfSnapA.length + ')</span>');
  if (_dfSnapB) parts.push('<span style="color:#f0a">B loaded (' + _dfSnapB.length + ')</span>');
  el.innerHTML = parts.join(' ');
}

async function dfRestoreOne(btn) {
  const addr = btn.dataset.addr;
  const size = btn.dataset.size;
  const value = btn.dataset.value;
  if (!(await confirmModal('Restore ' + addr + ' to ' + value + '? Battery must be unsealed.', {danger:true}))) return;
  btn.disabled = true;
  btn.textContent = '...';
  const fd = new FormData();
  fd.append('addr', addr); fd.append('value', value); fd.append('size', size);
  fetch('/api/batt/df/write', {method:'POST', body:fd}).then(r=>r.json()).then(j=>{
    btn.textContent = j.ok ? 'OK' : 'FAIL';
    btn.style.background = j.ok ? '#2a4' : '#a42';
    if (j.ok) setTimeout(()=>dfReadAll(), 500);
  });
}

function i2cPreflight() {
  const el = document.getElementById('preflightResult');
  el.style.display = 'block';
  el.textContent = 'Running preflight...';
  fetch('/api/i2c/preflight').then(r=>r.json()).then(j=>{
    let s = 'SDA: ' + (j.sdaOk ? 'OK' : 'STUCK LOW!') + '\n';
    s += 'SCL: ' + (j.sclOk ? 'OK' : 'STUCK LOW!') + '\n';
    s += 'Battery (0x0B): ' + (j.batteryAck ? 'ACK' : 'NO RESPONSE') + '\n';
    s += 'Devices found: ' + j.devCount;
    if (j.devices && j.devices.length) s += ' [' + j.devices.join(', ') + ']';
    el.textContent = s;
    el.style.color = (j.sdaOk && j.sclOk && j.batteryAck) ? '#0f0' : '#f44';
  });
}
function i2cScan() {
  const el = document.getElementById('i2cScanResult');
  el.style.display = 'block';
  el.textContent = 'Scanning...';
  fetch('/api/i2c/scan').then(r=>r.text()).then(t=>{ el.textContent = t; });
}

let _smbLogTimer = null;
function smbLogRefresh() {
  fetch('/api/smbus/log').then(r=>r.text()).then(t=>{
    document.getElementById('smbLog').textContent = t;
  });
}
function smbLogExport() {
  const text = document.getElementById('smbLog').textContent;
  const blob = new Blob([text], {type:'text/plain'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'smbus_log_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.txt';
  a.click();
}
function smbLogToggle() {
  fetch('/api/smbus/log/toggle', {method:'POST'}).then(r=>r.text()).then(t=>{
    document.getElementById('smbLogToggleBtn').textContent = t.includes('ON') ? 'Disable Logging' : 'Enable Logging';
    smbLogRefresh();
  });
}
function smbLogAutoToggle() {
  if (_smbLogTimer) { clearInterval(_smbLogTimer); _smbLogTimer = null; }
  if (document.getElementById('smbLogAuto').checked) {
    _smbLogTimer = setInterval(smbLogRefresh, 1000);
  }
}

function battSnapshot() {
  fetch('/api/batt/snapshot').then(r=>r.json()).then(j=>{
    const blob = new Blob([JSON.stringify(j, null, 2)], {type:'application/json'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'battery_snapshot_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.json';
    a.click();
  });
}

// === RC SNIFFER ===
let _rcPollTimer = null;
function rcStart(proto) {
  const fd = new FormData(); fd.append('proto', proto);
  fetch('/api/rc/start', {method:'POST', body:fd}).then(r=>r.text()).then(t=>{
    if (_rcPollTimer) clearInterval(_rcPollTimer);
    _rcPollTimer = setInterval(rcPoll, 250);
    rcPoll();
  });
}
function rcStop() {
  fetch('/api/rc/stop', {method:'POST'}).then(()=>{
    if (_rcPollTimer) { clearInterval(_rcPollTimer); _rcPollTimer = null; }
    document.getElementById('rcStatus').className='status off';
    document.getElementById('rcStatus').textContent='STOPPED';
    document.getElementById('rcConn').className='status off';
    document.getElementById('rcConn').textContent='NO LINK';
  });
}
function rcPoll() {
  fetch('/api/rc/state').then(r=>r.json()).then(s=>{
    const st = document.getElementById('rcStatus');
    st.className = 'status ' + (s.running ? 'on' : 'off');
    st.textContent = s.running ? 'RUNNING' : 'STOPPED';
    const co = document.getElementById('rcConn');
    co.className = 'status ' + (s.connected ? 'on' : 'off');
    co.textContent = s.connected ? 'LINK' : 'NO LINK';
    document.getElementById('rcProto').textContent = s.proto;
    document.getElementById('rcRate').textContent = s.frameRateHz + ' Hz';
    document.getElementById('rcFrames').textContent = s.frameCount + ' / ' + s.crcErrors;
    document.getElementById('rcFS').textContent = (s.failsafe ? 'FAILSAFE ' : '') + (s.lostFrame ? 'LOST' : s.failsafe ? '' : 'OK');
    // Render channels as bars
    let html = '';
    (s.channels || []).forEach((v, i) => {
      const pct = Math.max(0, Math.min(100, (v - 988) / (2012 - 988) * 100));
      const col = v < 900 || v > 2100 ? '#f44' : (v < 1000 || v > 2000 ? '#fa0' : '#4f4');
      html += `<div class="row" style="padding:2px 0"><span class="label" style="width:40px">Ch${i+1}</span><span class="value" style="width:60px;color:${col}">${v}μs</span><div class="bar" style="flex:1;margin-left:8px;height:10px"><div class="bar-fill" style="width:${pct}%;background:${col}"></div></div></div>`;
    });
    document.getElementById('rcChannels').innerHTML = html || '<div style="color:var(--text-dim);padding:10px">No channel data yet</div>';
  });
}

// === Receiver tab: live monitor (CRSF service start/stop) ===
// Full-featured per-RX param editor moved to rcv-config (RX Configuration
// card), which uses the one-shot /api/elrs/params path. The old CRSF-tab
// Ping/ReadParams/Bind/Commands cards were removed — all subsumed by the
// Receiver tab's Status / Configuration / Bind / Reboot sections.
function crsfStart() {
  const inv = document.getElementById('crsfInverted').checked ? '1' : '0';
  fetch('/api/crsf/start?inverted='+inv, {method:'POST'})
    .then(r=>r.text()).then(t=>showCmdResult(t));
}
function crsfStop() { fetch('/api/crsf/stop', {method:'POST'}).then(r=>r.text()).then(t=>showCmdResult(t)); }
function showCmdResult(msg) {
  // ctrlMsg is the shared status line in the Status card (Quick actions row).
  const el = document.getElementById('ctrlMsg');
  if (el) { el.textContent = (new Date().toLocaleTimeString()) + '  ' + msg; }
  else    { console.log('[ctrl]', msg); }
}
// Soft reboot via CRSF. Backend /api/crsf/reboot_app handles both states —
// if live monitor is running it uses the service; else one-shot Port B.
async function rcvSoftReboot() {
  if (!(await confirmModal('Reboot RX via CRSF? Link will drop briefly.'))) return;
  const el = document.getElementById('ctrlMsg');
  try {
    const r = await postForm('/api/crsf/reboot_app');
    if (el) el.textContent = '✓ ' + r;
    setTimeout(() => { try { rxProbeMode(); } catch(_) {} }, 4000);
  } catch (e) {
    if (el) el.textContent = '✗ ' + (e.message || e);
  }
}

// ===== Device identity: extract WiFi creds + UID from RX via fast NVS read =====
//
// Flow:  [Read identity] POSTs /api/elrs/identity/fast, backend reads NVS
//  (20KB @ 0x9000), OTADATA (8KB @ 0xe000), active-app tail (8KB) in one
//  DFU session, returns a concatenated hex response with per-section
//  headers. We parse all three client-side:
//   - NVS v2 walk → namespace "eeprom", key "eeprom" is an ELRS RxConfig
//     blob; UID lives at offset 4 (6 bytes) and flash_discriminator at 12.
//   - ELRSOPTS JSON at app-tail → wifi-ssid / wifi-password / uid (if
//     hasUID was baked at compile time) / domain / flash-discriminator.
//     Runtime override lives in SPIFFS /options.json — we don't read that
//     yet (Phase 2 full-dump fallback).
// WiFi AP defaults (not extractable from NVS/ELRSOPTS — they're compile-time
// constants in rodata): SSID = "ExpressLRS RX" / password = "expresslrs".

// --- Minimal MD5 (public-domain implementation, ~60 LOC) ---------------------
// Needed for phrase→UID conversion that matches ELRS Configurator
//   (UID = md5("-DMY_BINDING_PHRASE=\"<phrase>\"")[0..5]).
function _md5(str) {
  function add32(a,b){return (a+b)&0xffffffff;}
  function rol(n,s){return (n<<s)|(n>>>(32-s));}
  function cmn(q,a,b,x,s,t){return add32(rol(add32(add32(a,q),add32(x,t)),s),b);}
  function ff(a,b,c,d,x,s,t){return cmn((b&c)|((~b)&d),a,b,x,s,t);}
  function gg(a,b,c,d,x,s,t){return cmn((b&d)|(c&(~d)),a,b,x,s,t);}
  function hh(a,b,c,d,x,s,t){return cmn(b^c^d,a,b,x,s,t);}
  function ii(a,b,c,d,x,s,t){return cmn(c^(b|(~d)),a,b,x,s,t);}
  // UTF-8 encode
  const bytes = new TextEncoder().encode(str);
  const n = bytes.length;
  const nblk = ((n + 8) >> 6) + 1;
  const blks = new Int32Array(nblk * 16);
  for (let i = 0; i < n; i++) blks[i>>2] |= bytes[i] << ((i%4) * 8);
  blks[n>>2] |= 0x80 << ((n%4) * 8);
  blks[nblk*16 - 2] = n * 8;
  let a = 1732584193, b = -271733879, c = -1732584194, d = 271733878;
  for (let i = 0; i < blks.length; i += 16) {
    const oa=a, ob=b, oc=c, od=d;
    a=ff(a,b,c,d,blks[i+ 0], 7,-680876936); d=ff(d,a,b,c,blks[i+ 1],12,-389564586);
    c=ff(c,d,a,b,blks[i+ 2],17,  606105819); b=ff(b,c,d,a,blks[i+ 3],22,-1044525330);
    a=ff(a,b,c,d,blks[i+ 4], 7, -176418897); d=ff(d,a,b,c,blks[i+ 5],12, 1200080426);
    c=ff(c,d,a,b,blks[i+ 6],17,-1473231341); b=ff(b,c,d,a,blks[i+ 7],22, -45705983);
    a=ff(a,b,c,d,blks[i+ 8], 7, 1770035416); d=ff(d,a,b,c,blks[i+ 9],12,-1958414417);
    c=ff(c,d,a,b,blks[i+10],17,    -42063); b=ff(b,c,d,a,blks[i+11],22,-1990404162);
    a=ff(a,b,c,d,blks[i+12], 7, 1804603682); d=ff(d,a,b,c,blks[i+13],12,  -40341101);
    c=ff(c,d,a,b,blks[i+14],17,-1502002290); b=ff(b,c,d,a,blks[i+15],22, 1236535329);
    a=gg(a,b,c,d,blks[i+ 1], 5, -165796510); d=gg(d,a,b,c,blks[i+ 6], 9,-1069501632);
    c=gg(c,d,a,b,blks[i+11],14,  643717713); b=gg(b,c,d,a,blks[i+ 0],20, -373897302);
    a=gg(a,b,c,d,blks[i+ 5], 5, -701558691); d=gg(d,a,b,c,blks[i+10], 9,   38016083);
    c=gg(c,d,a,b,blks[i+15],14, -660478335); b=gg(b,c,d,a,blks[i+ 4],20, -405537848);
    a=gg(a,b,c,d,blks[i+ 9], 5,  568446438); d=gg(d,a,b,c,blks[i+14], 9,-1019803690);
    c=gg(c,d,a,b,blks[i+ 3],14, -187363961); b=gg(b,c,d,a,blks[i+ 8],20, 1163531501);
    a=gg(a,b,c,d,blks[i+13], 5,-1444681467); d=gg(d,a,b,c,blks[i+ 2], 9,  -51403784);
    c=gg(c,d,a,b,blks[i+ 7],14, 1735328473); b=gg(b,c,d,a,blks[i+12],20,-1926607734);
    a=hh(a,b,c,d,blks[i+ 5], 4,    -378558); d=hh(d,a,b,c,blks[i+ 8],11,-2022574463);
    c=hh(c,d,a,b,blks[i+11],16, 1839030562); b=hh(b,c,d,a,blks[i+14],23,  -35309556);
    a=hh(a,b,c,d,blks[i+ 1], 4,-1530992060); d=hh(d,a,b,c,blks[i+ 4],11, 1272893353);
    c=hh(c,d,a,b,blks[i+ 7],16, -155497632); b=hh(b,c,d,a,blks[i+10],23,-1094730640);
    a=hh(a,b,c,d,blks[i+13], 4,  681279174); d=hh(d,a,b,c,blks[i+ 0],11, -358537222);
    c=hh(c,d,a,b,blks[i+ 3],16, -722521979); b=hh(b,c,d,a,blks[i+ 6],23,   76029189);
    a=hh(a,b,c,d,blks[i+ 9], 4, -640364487); d=hh(d,a,b,c,blks[i+12],11, -421815835);
    c=hh(c,d,a,b,blks[i+15],16,  530742520); b=hh(b,c,d,a,blks[i+ 2],23, -995338651);
    a=ii(a,b,c,d,blks[i+ 0], 6, -198630844); d=ii(d,a,b,c,blks[i+ 7],10, 1126891415);
    c=ii(c,d,a,b,blks[i+14],15,-1416354905); b=ii(b,c,d,a,blks[i+ 5],21,  -57434055);
    a=ii(a,b,c,d,blks[i+12], 6, 1700485571); d=ii(d,a,b,c,blks[i+ 3],10,-1894986606);
    c=ii(c,d,a,b,blks[i+10],15,   -1051523); b=ii(b,c,d,a,blks[i+ 1],21,-2054922799);
    a=ii(a,b,c,d,blks[i+ 8], 6, 1873313359); d=ii(d,a,b,c,blks[i+15],10,  -30611744);
    c=ii(c,d,a,b,blks[i+ 6],15,-1560198380); b=ii(b,c,d,a,blks[i+13],21, 1309151649);
    a=ii(a,b,c,d,blks[i+ 4], 6, -145523070); d=ii(d,a,b,c,blks[i+11],10,-1120210379);
    c=ii(c,d,a,b,blks[i+ 2],15,  718787259); b=ii(b,c,d,a,blks[i+ 9],21, -343485551);
    a=add32(a,oa); b=add32(b,ob); c=add32(c,oc); d=add32(d,od);
  }
  // Serialise little-endian
  const out = new Uint8Array(16);
  [a,b,c,d].forEach((v,i)=>{
    out[i*4+0]=v&0xff; out[i*4+1]=(v>>>8)&0xff; out[i*4+2]=(v>>>16)&0xff; out[i*4+3]=(v>>>24)&0xff;
  });
  return out;
}

function identPhraseToUid() {
  const phrase = document.getElementById('phraseInput').value;
  if (!phrase) { document.getElementById('phraseUid').textContent = '—'; return; }
  const full = '-DMY_BINDING_PHRASE="' + phrase + '"';
  const digest = _md5(full);
  const uid = Array.from(digest.slice(0, 6)).map(b => b.toString(16).padStart(2,'0')).join(':').toUpperCase();
  document.getElementById('phraseUid').textContent = uid;
}

// --- NVS v2 walker (ported from hardware/bayckrc_c3_dual/parse.py) ----------
function _nvsParse(bytes) {
  const PAGE = 4096, HDR = 32, ESZ = 32, NENT = 126;
  const STATE_ACTIVE = 0xFFFFFFFE, STATE_FULL = 0xFFFFFFFC, STATE_FREEING = 0xFFFFFFF8;
  const T_U8=0x01, T_U32=0x04, T_STR=0x21, T_BLOB_DATA=0x42, T_BLOB_IDX=0x48, T_BLOB_V2=0x41;
  const namespaces = {};
  const entries = {};  // { ns_name: { key: { type, value, value_hex } } }
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  function u32(off) { return dv.getUint32(off, true); }
  function u16(off) { return dv.getUint16(off, true); }

  for (const pass of [1, 2]) {
    for (let pageBase = 0; pageBase + PAGE <= bytes.length; pageBase += PAGE) {
      const state = u32(pageBase);
      if (state !== STATE_ACTIVE && state !== STATE_FULL && state !== STATE_FREEING) continue;
      let eidx = 0;
      while (eidx < NENT) {
        const eOff = pageBase + HDR + eidx * ESZ;
        if (eOff + ESZ > pageBase + PAGE) break;
        const nsId = bytes[eOff];
        const eType = bytes[eOff + 1];
        let span = bytes[eOff + 2];
        // blank / erased
        if (nsId === 0xFF) { eidx++; continue; }
        if (span === 0 || span > NENT) span = 1;
        // key slice 8..24, ASCII clean
        let keyEnd = eOff + 24;
        for (let i = eOff + 8; i < eOff + 24; i++) if (bytes[i] === 0) { keyEnd = i; break; }
        const keyBytes = bytes.slice(eOff + 8, keyEnd);
        const printable = Array.from(keyBytes).every(b => b >= 0x20 && b < 0x7F);
        if (!printable) { eidx += span; continue; }
        const key = new TextDecoder('utf-8', {fatal:false}).decode(keyBytes);
        if (pass === 1) {
          if (nsId === 0 && eType === T_U8 && key) namespaces[bytes[eOff + 24]] = key;
          eidx += span; continue;
        }
        if (nsId === 0 || !key) { eidx += span; continue; }
        const nsName = namespaces[nsId] || ('ns#' + nsId);
        let value = null, value_hex = null;
        try {
          if (eType === T_U8) value = bytes[eOff + 24];
          else if (eType === T_U32) value = u32(eOff + 24);
          else if (eType === T_STR) {
            const sz = u16(eOff + 24);
            const ds = eOff + ESZ;
            if (ds + sz <= pageBase + PAGE) {
              const raw = bytes.slice(ds, ds + sz);
              const nul = raw.indexOf(0);
              value = new TextDecoder().decode(nul < 0 ? raw : raw.slice(0, nul));
            }
          }
          else if (eType === T_BLOB_DATA || eType === T_BLOB_V2) {
            const sz = u16(eOff + 24);
            const ds = eOff + ESZ;
            const end = Math.min(ds + sz, pageBase + PAGE, bytes.length);
            const raw = bytes.slice(ds, end);
            value = { size: sz, length: raw.length };
            value_hex = Array.from(raw).map(b=>b.toString(16).padStart(2,'0')).join('');
          }
        } catch (e) { /* ignore */ }
        entries[nsName] = entries[nsName] || {};
        entries[nsName][key] = { type: eType, value, value_hex };
        eidx += span;
      }
    }
  }
  return { namespaces, entries };
}

// Shared device profile (Phase M1). Populated from partition table + OTADATA
// on successful Identity read and reused elsewhere (e.g. dynamic Flash targets,
// dump-size presets). Kept on window so it survives across modal code paths.
window._rxProfile = window._rxProfile || null;

// ZLRS fork signature scan — runs over any rodata blob we have (app tails,
// sometimes NVS if the SSID got persisted). Returns null for vanilla ELRS.
//   research/ZLRS_3_36_ANALYSIS.md §6 has the full signature table.
function _detectFork(bytes) {
  const dec = new TextDecoder('utf-8', {fatal:false});
  const s = dec.decode(bytes);
  // Order matters — most specific first.
  if (s.indexOf('zlrs24022022') >= 0) return { name: 'ZLRS 3.36', basis: 'ExpressLRS 3.6.x' };
  if (s.indexOf('3.36 (2110c5)') >= 0) return { name: 'ZLRS 3.36', basis: 'ExpressLRS 3.6.x' };
  if (s.indexOf('Приёмник_ZLRS') >= 0) return { name: 'ZLRS RX', basis: 'ExpressLRS' };
  if (s.indexOf('Передатчик_НСУ') >= 0) return { name: 'ZLRS TX', basis: 'ExpressLRS' };
  if (s.indexOf('MILELRS') >= 0) return { name: 'MILELRS', basis: 'ExpressLRS' };
  if (s.indexOf('ExpressLRS') >= 0) return { name: 'ExpressLRS', basis: null };
  return null;
}

// Minimal partition-table parser — ESP-IDF v1, little-endian, entries of 32 B
// at 0x8000. Each: magic 0xAA50, type u8, subtype u8, offset u32, size u32,
// label char[16], flags u32. We use this both for Phase M1 device profile
// and to resolve app0/app1 end offsets when they differ from the default
// Layout A. Invalid or all-0xFF entries stop iteration.
function _parsePartitionTable(bytes) {
  const parts = [];
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i + 32 <= bytes.length; i += 32) {
    if (bytes[i] !== 0xAA || bytes[i+1] !== 0x50) break;
    const type = bytes[i+2];
    const subtype = bytes[i+3];
    const off = dv.getUint32(i+4, true);
    const sz  = dv.getUint32(i+8, true);
    const labelEnd = bytes.subarray(i+12, i+28).indexOf(0);
    const label = new TextDecoder().decode(
        bytes.subarray(i+12, i+12 + (labelEnd < 0 ? 16 : labelEnd)));
    parts.push({ type, subtype, offset: off, size: sz, label });
  }
  return parts;
}

async function identRead(thorough) {
  const btn = document.getElementById('identReadBtn');
  const btn2 = document.getElementById('identReadThoroughBtn');
  const st = document.getElementById('identStatus');
  const active = thorough ? btn2 : btn;
  const origLabel = active.textContent;
  btn.disabled = true; btn2.disabled = true;
  active.textContent = thorough ? '⏳ Reading (~24 s)…' : '⏳ Reading (~8 s)…';
  st.textContent = thorough
    ? 'Requesting PT + NVS + OTADATA + both app tails + SPIFFS via ROM DFU…'
    : 'Requesting partition table + NVS + OTADATA + both app tails via ROM DFU…';
  try {
    const fd = new FormData();
    if (thorough) fd.append('spiffs', '1');
    const r = await fetch('/api/elrs/identity/fast', {method:'POST', body: thorough ? fd : undefined});
    const txt = await r.text();
    if (!r.ok) throw new Error(txt || ('HTTP ' + r.status));
    // Parse sectional response: header "# section=NAME off=... size=..." then
    // one line of hex. Up to 5 sections: PT, NVS, OTADATA, APP0TAIL, APP1TAIL.
    const sections = {};
    let activeSlot = 0, maxSeq = 0;
    const lines = txt.split('\n');
    let i = 0;
    while (i < lines.length) {
      const m = lines[i].match(/^# section=(\w+) off=(0x[0-9a-f]+) size=(0x[0-9a-f]+)(?: active_slot=(\d+))?(?: max_seq=(\d+))?/i);
      if (m && i + 1 < lines.length) {
        const name = m[1]; const off = parseInt(m[2]); const size = parseInt(m[3]);
        if (m[4] !== undefined) activeSlot = +m[4];
        if (m[5] !== undefined) maxSeq = +m[5];
        const hex = lines[i + 1].trim();
        const u8 = new Uint8Array(size);
        for (let j = 0; j < size && j * 2 + 1 < hex.length; j++) {
          u8[j] = parseInt(hex.substr(j * 2, 2), 16);
        }
        sections[name] = { off, size, bytes: u8 };
        i += 2;
      } else {
        i += 1;
      }
    }

    // Phase M1: decode partition table, build shared profile.
    let parts = [];
    if (sections.PT) {
      parts = _parsePartitionTable(sections.PT.bytes);
      const app0 = parts.find(p => p.type === 0 && p.subtype === 0x10);
      const app1 = parts.find(p => p.type === 0 && p.subtype === 0x11);
      const nvs  = parts.find(p => p.type === 1 && p.subtype === 0x02);
      const spiffs = parts.find(p => p.type === 1 && (p.subtype === 0x82 || p.label === 'spiffs'));
      window._rxProfile = {
        partitions: parts,
        app0, app1, nvs, spiffs,
        active_slot: activeSlot, max_seq: maxSeq,
        has_dual_slot: !!(app0 && app1),
        flash_end: parts.length
          ? Math.max(...parts.map(p => p.offset + p.size))
          : 0x400000,
      };
    }

    // Parse NVS: the RxConfig blob lives in namespace "eeprom", key "eeprom".
    // rx_config_t layout:
    //   0..3   version (u32 LE)
    //   4..9   uid[6]
    //   10     unused_padding
    //   11     serial1Protocol packed
    //   12..15 flash_discriminator (u32 LE)
    let uid = null, flashDisc = null, bound = null;
    let ssid = null, password = null;
    if (sections.NVS) {
      const nvs = _nvsParse(sections.NVS.bytes);
      const blob = nvs.entries?.eeprom?.eeprom;
      if (blob && blob.value_hex) {
        const hex = blob.value_hex;
        uid = Array.from({length:6}, (_,k) =>
          hex.substr((4+k)*2, 2).toUpperCase()).join(':');
        bound = (uid === '00:00:00:00:00:00') ? 'no (zero UID)' : 'yes (NVS, runtime)';
        const d0 = parseInt(hex.substr(24,2),16), d1 = parseInt(hex.substr(26,2),16);
        const d2 = parseInt(hex.substr(28,2),16), d3 = parseInt(hex.substr(30,2),16);
        flashDisc = '0x' + ((d3<<24)|(d2<<16)|(d1<<8)|d0).toString(16).padStart(8,'0');
      }
      const elrsNs = nvs.entries?.ELRS;
      if (!uid && elrsNs) {
        if (elrsNs.tx_version) bound = 'TX config present (UID in SPIFFS /options.json — needs full dump)';
      }
    }

    // Parse ELRSOPTS JSON from the ACTIVE app tail. Phase 1b: we got both
    // tails, pick the one matching OTADATA's active slot.
    const tailKey = activeSlot === 1 ? 'APP1TAIL' : 'APP0TAIL';
    let tailBytes = sections[tailKey]?.bytes || sections.APP0TAIL?.bytes;
    if (tailBytes) {
      const dec = new TextDecoder('utf-8', {fatal:false});
      const txtBlob = dec.decode(tailBytes);
      const markers = ['"wifi-ssid"','"wifi-password"','"flash-discriminator"','"uid"'];
      for (const m of markers) {
        const mi = txtBlob.indexOf(m);
        if (mi < 0) continue;
        let j = mi;
        while (j > 0 && txtBlob.charCodeAt(j) !== 123) j--;
        let depth = 0, end = -1;
        for (let k = j; k < Math.min(j + 4096, txtBlob.length); k++) {
          const c = txtBlob.charCodeAt(k);
          if (c === 123) depth++;
          else if (c === 125) { depth--; if (depth === 0) { end = k + 1; break; } }
        }
        if (end < 0) continue;
        try {
          const obj = JSON.parse(txtBlob.slice(j, end));
          if (obj['wifi-ssid'])     ssid = obj['wifi-ssid'];
          if (obj['wifi-password']) password = obj['wifi-password'];
          if (!uid && Array.isArray(obj.uid) && obj.uid.length === 6) {
            uid = obj.uid.map(x => (x&0xff).toString(16).padStart(2,'0')).join(':').toUpperCase();
            bound = 'yes (compile-time baked)';
          }
          if (!flashDisc && obj['flash-discriminator'])
            flashDisc = '0x' + obj['flash-discriminator'].toString(16).padStart(8,'0');
          break;
        } catch (e) { /* keep trying other markers */ }
      }
    }

    // Fork detection — scan ALL tails, not just active (fork strings may be
    // in rodata of a vanilla image too if fork upgraded from it).
    let fork = null;
    for (const key of ['APP0TAIL', 'APP1TAIL']) {
      if (sections[key]) {
        const f = _detectFork(sections[key].bytes);
        if (f && (!fork || f.name.startsWith('ZLRS') || f.name.startsWith('MILELRS'))) fork = f;
      }
    }
    if (fork && window._rxProfile) window._rxProfile.fork = fork;

    // Phase 1c: SPIFFS scan — extract /options.json runtime overrides.
    // ELRS RX firmware writes a JSON file with the keys it cares about
    // (wifi-ssid / wifi-password / uid / tlm-interval / etc). The file
    // lives somewhere inside the ~192 KB partition; rather than parse the
    // SPIFFS / LittleFS directory we scan the raw bytes for the key
    // markers and extract the surrounding JSON object — same trick as
    // the app-tail parser above. Hits in SPIFFS are RUNTIME values, so
    // they take precedence over compile-time defaults from the app tail.
    let txUid = null, staSsid = null, staPass = null;
    if (sections.SPIFFS) {
      const dec = new TextDecoder('utf-8', {fatal:false});
      const sp = dec.decode(sections.SPIFFS.bytes);
      const markers = ['"wifi-ssid"','"wifi-password"','"uid"'];
      for (const m of markers) {
        let from = 0;
        while (from < sp.length) {
          const mi = sp.indexOf(m, from);
          if (mi < 0) break;
          // Walk back to nearest '{' and forward to matching '}'.
          let j = mi;
          while (j > 0 && sp.charCodeAt(j) !== 123) j--;
          let depth = 0, end = -1;
          for (let k = j; k < Math.min(j + 4096, sp.length); k++) {
            const c = sp.charCodeAt(k);
            if (c === 123) depth++;
            else if (c === 125) { depth--; if (depth === 0) { end = k + 1; break; } }
          }
          if (end < 0) { from = mi + m.length; continue; }
          try {
            const obj = JSON.parse(sp.slice(j, end));
            if (obj['wifi-ssid'] && !staSsid) staSsid = obj['wifi-ssid'];
            if (obj['wifi-password'] && !staPass) staPass = obj['wifi-password'];
            if (Array.isArray(obj.uid) && obj.uid.length === 6 && !txUid) {
              txUid = obj.uid.map(x => (x&0xff).toString(16).padStart(2,'0')).join(':').toUpperCase();
            }
            from = end;
            break;  // first valid object with this marker is enough
          } catch (e) {
            from = mi + m.length;
          }
        }
      }
      // Show the new rows only when SPIFFS was actually scanned, even if
      // some fields stayed empty (signals "we looked, nothing found").
      document.getElementById('identTxUidRow').style.display = '';
      document.getElementById('identStaSsidRow').style.display = '';
      document.getElementById('identStaPassRow').style.display = '';
      document.getElementById('identTxUid').textContent  = txUid  || '— (TX UID not in /options.json — RX bound at runtime?)';
      document.getElementById('identStaSsid').textContent = staSsid || '— (no STA override — RX in AP mode)';
      document.getElementById('identStaPass').textContent = staPass || '—';
    }

    document.getElementById('identUid').textContent  = uid || '— (not bound / not found in NVS)';
    document.getElementById('identBound').textContent = bound || 'unknown';
    document.getElementById('identSsid').textContent = ssid || 'ExpressLRS RX  (compile-time default)';
    document.getElementById('identPass').textContent = password || 'expresslrs  (compile-time default)';
    document.getElementById('identDisc').textContent = flashDisc || '—';

    // Fork badge — create on-demand after first identity read.
    let forkRow = document.getElementById('identForkRow');
    if (fork) {
      if (!forkRow) {
        const card = document.getElementById('identCard');
        forkRow = document.createElement('div');
        forkRow.id = 'identForkRow';
        forkRow.className = 'row';
        forkRow.innerHTML = '<span class="label">Firmware fork:</span><span class="value" id="identFork" style="color:var(--accent)">—</span>';
        // Insert before the read button (which is the last non-details child).
        const btnEl = document.getElementById('identReadBtn');
        card.insertBefore(forkRow, btnEl);
      }
      const label = fork.name + (fork.basis ? '  (based on ' + fork.basis + ')' : '');
      document.getElementById('identFork').textContent = label;
    } else if (forkRow) {
      forkRow.remove();
    }

    // Invalidate Flash Firmware target dropdown so it picks up profile offsets.
    const fwTarget = document.getElementById('fwTarget');
    if (fwTarget) { fwTarget._profileApplied = false; fwPathUpdate(); }

    let msg = '✓ Identity read complete';
    if (window._rxProfile && parts.length) {
      msg += `  (partition table: ${parts.length} entries, `
          + `flash ${(window._rxProfile.flash_end/1048576).toFixed(1)} MB, `
          + `active slot: app${activeSlot})`;
    }
    if (sections.SPIFFS) {
      msg += `  · SPIFFS: ${(sections.SPIFFS.size/1024)|0} KB scanned`;
    }
    st.textContent = msg;
    st.style.color = '#0f0';
    // Hide the "put RX in DFU" hint once we got a result.
    const ph = document.getElementById('identPlaceholder');
    if (ph) ph.style.display = 'none';
  } catch (e) {
    st.textContent = '✗ ' + (e.message || e);
    st.style.color = '#f66';
  } finally {
    btn.disabled = false; btn2.disabled = false;
    active.textContent = origLabel;
  }
}

// === Mode-aware button gating ===
// Every action button that has a mode prerequisite declares data-need-mode:
//   "app"      — RX must be running firmware (app mode)
//   "dfu"      — RX must be in ROM DFU (BOOT + power-cycle)
//   "dfustub"  — either DFU or in-app stub flasher
// When Probe reports a mode, we disable mismatched buttons and set a tooltip
// explaining why. Mode "silent" or unknown → leave enabled (probing needed).
// Called from rxProbeMode success handler + on tab activation.
function applyRxModeGating(mode) {
  if (!mode) mode = '';
  const need = {
    app:      (m) => m === 'app',
    dfu:      (m) => m === 'dfu',
    dfustub:  (m) => m === 'dfu' || m === 'stub',
  };
  const hint = {
    app:      'RX must be running its firmware (app mode). Probe first.',
    dfu:      'RX must be in ROM DFU — hold BOOT while power-cycling RX.',
    dfustub:  'RX must be in DFU or stub flasher.',
  };
  document.querySelectorAll('[data-need-mode]').forEach(btn => {
    const want = btn.getAttribute('data-need-mode');
    const check = need[want];
    if (!check) return;
    // Unknown mode → don't disable (user may want to try probing by pressing).
    if (!mode || mode === 'silent' || mode === 'unknown') {
      btn.disabled = false;
      btn.classList.remove('mode-blocked');
      btn._origTitle = btn._origTitle || btn.title;
      btn.title = btn._origTitle;
      return;
    }
    btn._origTitle = btn._origTitle || btn.title;
    if (check(mode)) {
      btn.disabled = false;
      btn.classList.remove('mode-blocked');
      btn.title = btn._origTitle;
    } else {
      btn.disabled = true;
      btn.classList.add('mode-blocked');
      btn.title = hint[want] + (btn._origTitle ? '\n— ' + btn._origTitle : '');
    }
  });
}

// === Receiver tab: action-picker accordion ===
// Click a nav button → matching section opens, all others close. Click
// again → close the open one. Only 3 workflow sections remain (flash,
// config, live); Bind + Reboot moved to the Status card's Quick actions.
function rcvOpen(section) {
  const target = document.getElementById('rcv-' + section);
  const wasOpen = target && target.style.display !== 'none';
  // Anchor: record viewport-relative Y of the picker BEFORE layout changes,
  // then restore that offset AFTER. Without this the page "jumps" as sections
  // of different heights swap, because the browser keeps scroll offset but
  // content above/below the viewport shifts.
  const picker = document.querySelector('button[data-rcv]');
  const anchorRect = picker ? picker.getBoundingClientRect() : null;

  document.querySelectorAll('.rcv-section').forEach(e => e.style.display = 'none');
  document.querySelectorAll('button[data-rcv]').forEach(b => b.classList.remove('active'));
  if (!wasOpen && target) {
    target.style.display = '';
    const nav = document.querySelector('button[data-rcv="' + section + '"]');
    if (nav) nav.classList.add('active');
    // Section-specific post-open init
    if (section === 'flash')  fwPathUpdate();
    if (section === 'config') { /* user clicks Load manually */ }
    if (section === 'live')   { /* CRSF starts on explicit click */ }
  }

  // Restore the picker's viewport position so the content below it is
  // predictable. Defers to next frame so layout has settled.
  if (anchorRect) {
    requestAnimationFrame(() => {
      const newRect = picker.getBoundingClientRect();
      const delta   = newRect.top - anchorRect.top;
      if (Math.abs(delta) > 1) window.scrollBy({top: delta, left: 0, behavior: 'instant'});
    });
  }
}

// === ELRS FLASH ===
// Shared fetch helper — throws on HTTP error so callers can't confuse a
// 4xx/5xx with success. fetch() alone resolves on any HTTP response, so
// plain `.then(r=>r.text())` silently swallows "Port B busy" 409s.
async function postForm(url, form) {
  const r = await fetch(url, form ? {method:'POST', body: form} : {method:'POST'});
  const t = await r.text();
  if (!r.ok) throw new Error('HTTP ' + r.status + ': ' + (t || r.statusText));
  return t;
}

// Async drop-in replacement for browser confirm(). Returns Promise<boolean>.
// Use as: if (!(await confirmModal('msg'))) return;
//   opts.danger=true   — red OK button + red modal border
//   opts.okLabel='Run' — custom OK label
let _modalResolve = null;
function _modalCancel() { _modalClose(false); }
function _modalOk()     { _modalClose(true); }
function _modalClose(result) {
  document.getElementById('confirmBackdrop').classList.remove('show');
  if (_modalResolve) { _modalResolve(result); _modalResolve = null; }
}
function confirmModal(msg, opts) {
  opts = opts || {};
  return new Promise(resolve => {
    document.getElementById('confirmMsg').textContent = msg;
    const okBtn = document.getElementById('confirmOkBtn');
    okBtn.textContent = opts.okLabel || 'OK';
    okBtn.classList.toggle('danger', !!opts.danger);
    okBtn.classList.toggle('success', !opts.danger);
    document.getElementById('confirmBox').classList.toggle('danger', !!opts.danger);
    document.getElementById('confirmBackdrop').classList.add('show');
    setTimeout(() => okBtn.focus(), 30);
    _modalResolve = resolve;
  });
}
// Esc to cancel, Enter to accept — matches native confirm() shortcuts.
document.addEventListener('keydown', e => {
  if (!document.getElementById('confirmBackdrop').classList.contains('show')) return;
  if (e.key === 'Escape') _modalCancel();
  else if (e.key === 'Enter') _modalOk();
});
async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error('HTTP ' + r.status + ': ' + (await r.text()));
  return r.json();
}

// ===== RX mode detector =====
// Probes RX on Port B for current operational state. Runs once when ELRS
// tab opens + on demand via button. Result gates which flash buttons are
// enabled (Stub-flash needs mode='app', DFU flash wants 'dfu').
let _rxMode = 'unknown';
function rxApplyModeGate() {
  // Most buttons are gated declaratively via data-need-mode — apply them first.
  applyRxModeGating(_rxMode);
  // Dynamically-generated per-slot flash buttons (rendered from rxScan output)
  // don't have a static data-need-mode, so gate them here.
  document.querySelectorAll('button[onclick^="rxFlashStub"]').forEach(b => {
    b.disabled = (_rxMode !== 'app' && _rxMode !== 'stub');
    b.title = b.disabled
      ? 'Stub-flash requires RX in app or already in stub — current mode: ' + _rxMode
      : 'Auto-flash via in-app ELRS stub @420000';
  });
  document.querySelectorAll('button[onclick^="rxFlashToSlot"]').forEach(b => {
    b.disabled = (_rxMode !== 'dfu');
    b.title = b.disabled
      ? 'Flash (DFU) requires RX in ROM DFU — current mode: ' + _rxMode
      : 'ROM DFU flash @115200';
  });
}
let _rxProbeAt = 0;
function rxAgeTick() {
  const el = document.getElementById('rxModeAge');
  if (!el || !_rxProbeAt) return;
  const age = Math.floor((Date.now() - _rxProbeAt) / 1000);
  el.textContent = age < 5 ? '' : ('probed ' + age + 's ago' + (age > 45 ? ' — stale' : ''));
  el.style.color = age > 45 ? '#f80' : 'var(--text-dim)';
}
if (!window._rxAgeTimer) window._rxAgeTimer = setInterval(rxAgeTick, 3000);
async function rxProbeMode() {
  const btn = document.getElementById('rxProbeBtn');
  const badge = document.getElementById('rxModeBadge');
  const hint = document.getElementById('rxModeHint');
  btn.disabled = true; btn.textContent = '⏳ Probing (~1s)…';
  badge.innerHTML = '⋯ <b>Probing</b>';
  badge.setAttribute('aria-label', 'Receiver mode: probing');
  badge.style.background = '#444';
  badge.style.color = '#ccc';
  // Clear identity rows
  for (const id of ['rxName','rxVer','rxIds','rxFields']) {
    const el = document.getElementById(id);
    if (el) el.textContent = '—';
  }
  try {
    const txt = await postForm('/api/elrs/rx_mode');
    const d = JSON.parse(txt);
    _rxMode = d.mode;
    _rxProbeAt = Date.now();
    // Shape-prefix for colour-blind redundancy: ◆/◇/●/○ pair with the
    // semantic colour so mode is distinguishable on monochrome displays.
    const colors = {
      'dfu':    {bg:'#c60',   fg:'#fff', icon:'◆', tag:'DFU',    detail:'ROM @115200'},
      'stub':   {bg:'#06c',   fg:'#fff', icon:'◇', tag:'STUB',   detail:'@420000'},
      'app':    {bg:'#0a3',   fg:'#fff', icon:'●', tag:'APP',    detail:'running'},
      'silent': {bg:'#633',   fg:'#fcc', icon:'○', tag:'SILENT', detail:'WiFi/halted'},
    };
    const c = colors[d.mode] || {bg:'#333', fg:'#ccc', icon:'?', tag:d.mode, detail:''};
    badge.innerHTML = c.icon + ' <b>' + c.tag + '</b>' +
                      (c.detail ? ' <span style="font-weight:normal;opacity:0.85">(' + c.detail + ')</span>' : '');
    badge.setAttribute('aria-label', 'Receiver mode: ' + c.tag + (c.detail ? ', ' + c.detail : ''));
    badge.style.background = c.bg;
    badge.style.color = c.fg;
    hint.textContent = d.hint || '';
    rxAgeTick();
    if (d.app_ok && d.app) {
      document.getElementById('rxName').textContent = d.app.name || '—';
      const sv = d.app.sw_version || 0;
      const verStr = ((sv >>> 24) & 0xff) + '.' + ((sv >>> 16) & 0xff) + '.' + ((sv >>> 8) & 0xff) + '.' + (sv & 0xff);
      document.getElementById('rxVer').textContent = verStr + '  (raw 0x' + sv.toString(16).padStart(8, '0') + ')';
      document.getElementById('rxIds').textContent = 'serial=' + d.app.serial_no + '  hw=0x' + d.app.hw_id.toString(16);
      document.getElementById('rxFields').textContent = d.app.field_count + ' (param_ver ' + d.app.parameter_version + ')';
    } else if (d.mode === 'dfu' || d.mode === 'stub') {
      // BUG-ID2 fix: CRSF DEVICE_PING only works when RX is in app mode, so
      // the identity fields stay empty in DFU/stub. Previously users saw
      // unexplained "—" rows. Add an explicit hint + pull from Identity /
      // Scan cache if available.
      const profile = window._rxProfile;
      if (profile && profile.partitions && profile.partitions.length) {
        document.getElementById('rxName').textContent =
            (profile.fork?.name || 'ELRS-family') +
            '  (from Identity read)';
        document.getElementById('rxIds').textContent =
            `flash ${(profile.flash_end/1048576).toFixed(1)} MB, ` +
            `${profile.partitions.length} partitions, ` +
            `slot app${profile.active_slot} active`;
      }
      hint.textContent = (d.hint || '') +
          '  ℹ Firmware name/version need RX in app mode (CRSF DEVICE_PING). ' +
          (profile ? 'Profile loaded from Identity — use Flash or exit DFU to refresh.' : 'Read identity below or exit DFU to populate.');
    }
    rxApplyModeGate();
    fwPathUpdate();
  } catch (e) {
    _rxMode = 'error';
    badge.innerHTML = '✗ <b>Error</b>';
    badge.setAttribute('aria-label', 'Receiver mode: probe failed');
    badge.style.background = '#900';
    badge.style.color = '#fff';
    hint.textContent = (e.message || e);
  } finally {
    btn.disabled = false; btn.textContent = '🔍 Probe RX';
  }
}

// ===== Flash Firmware unified flow =====
let _fwCached = null;  // { name, size } tracker for PSRAM buffer
function fwSourceChange() {
  const src = document.getElementById('fwSource').value;
  document.getElementById('fwCatalogBox').style.display = src === 'catalog' ? 'block' : 'none';
  document.getElementById('fwUploadBox').style.display  = src === 'upload'  ? 'block' : 'none';
}
function fwPopulateModels() {
  const m = document.getElementById('fwModel');
  if (!m || m.options.length) return;
  if (!window.ELRS_MODELS) return;
  for (const model of ELRS_MODELS) {
    const opt = document.createElement('option');
    opt.value = model.id;
    opt.textContent = model.name + ' [' + model.role + ' · ' + model.chip + ']';
    opt.dataset.fw = model.fw;
    m.appendChild(opt);
  }
  m.value = 'bayck.rx_dual.dualc3';
}
function fwPathUpdate() {
  const targetSel = document.getElementById('fwTarget');

  // Phase M1: if we have a device profile (from Identity read), rebuild the
  // target options with REAL partition offsets + sizes. Before: hardcoded
  // 0x10000/0x1F0000 labels that lied on S3 TX modules with different layouts.
  const profile = window._rxProfile;
  if (profile && profile.partitions && profile.partitions.length && !targetSel._profileApplied) {
    const prevValue = targetSel.value;
    targetSel.innerHTML = '';
    const mkOpt = (val, label) => {
      const o = document.createElement('option');
      o.value = val; o.textContent = label;
      targetSel.appendChild(o);
      return o;
    };
    if (profile.app0) {
      const mb = (profile.app0.size / 1048576).toFixed(2);
      mkOpt('0x' + profile.app0.offset.toString(16),
            `app0 @0x${profile.app0.offset.toString(16)} (${mb} MB) ${profile.active_slot === 0 ? '✓ active' : ''}`);
    }
    if (profile.app1) {
      const mb = (profile.app1.size / 1048576).toFixed(2);
      mkOpt('0x' + profile.app1.offset.toString(16),
            `app1 @0x${profile.app1.offset.toString(16)} (${mb} MB) ${profile.active_slot === 1 ? '✓ active' : ''}`);
    }
    mkOpt('0x0', 'full image @0x0 (incl. bootloader — ROM DFU only)');
    // Restore prior selection if still valid; else pick active slot.
    if (Array.from(targetSel.options).some(o => o.value === prevValue)) {
      targetSel.value = prevValue;
    } else if (profile.app0 && profile.active_slot === 0) {
      targetSel.value = '0x' + profile.app0.offset.toString(16);
    }
    targetSel._profileApplied = true;
  }

  const target = parseInt(targetSel.value, 16);
  const path = document.getElementById('fwPath');
  const btn  = document.getElementById('fwFlashBtn');
  const bootLabel = document.getElementById('fwBootAfter')?.parentElement?.parentElement;

  // Gate: "full image @0x0" only makes sense in ROM DFU (stub can't touch bootloader).
  // If mode != dfu and user had full-image selected, auto-switch to app0.
  const fullOpt = targetSel.querySelector('option[value="0x0"]');
  if (fullOpt) fullOpt.disabled = (_rxMode !== 'dfu');
  if (target === 0 && _rxMode !== 'dfu' && _rxMode !== 'unknown') {
    const app0Val = profile?.app0 ? '0x' + profile.app0.offset.toString(16) : '0x10000';
    targetSel.value = app0Val;
  }

  // Hide "flip OTADATA after flash" for full-image target. Slot target = any
  // non-zero offset that matches a detected app partition.
  if (bootLabel) {
    const app0Off = profile?.app0?.offset;
    const app1Off = profile?.app1?.offset;
    const isSlotTarget = (target === app0Off || target === app1Off ||
                          target === 0x10000 || target === 0x1f0000);
    bootLabel.closest('.row').style.display = isSlotTarget ? '' : 'none';
  }

  let pathText = '', pathOk = false;
  if (target === 0) {
    if (_rxMode === 'dfu') { pathText = 'ROM DFU @115200 — full flash (bootloader + partitions + app)'; pathOk = true; }
    else                   { pathText = 'requires ROM DFU — put RX in DFU (hold BOOT + power-cycle)'; pathOk = false; }
  } else {
    if (_rxMode === 'app' || _rxMode === 'stub') {
      pathText = 'in-app stub @420000 — no physical buttons needed';
      pathOk = true;
    } else if (_rxMode === 'dfu') {
      pathText = 'ROM DFU @115200 — RX already in DFU';
      pathOk = true;
    } else {
      pathText = 'RX is ' + _rxMode + ' — Flash unavailable; Probe first';
      pathOk = false;
    }
  }
  path.textContent = pathText;
  btn.disabled = !pathOk || !_fwCached;
  btn.title = !_fwCached ? 'Cache firmware first (Download or Upload)' : pathText;
}
async function fwFetch() {
  const status = document.getElementById('fwResult');
  const btn = document.getElementById('fwFetchBtn');
  btn.disabled = true; btn.textContent = '⏳ Working…';
  try {
    const version = document.getElementById('fwVersion').value;
    const variant = document.getElementById('fwVariant').value;
    const modelId = document.getElementById('fwModel').value;
    const model   = ELRS_MODELS.find(m => m.id === modelId);
    if (!model) throw new Error('no model selected');
    const rel = ELRS_RELEASES.find(r => r.version === version);
    if (!rel) throw new Error('no release ' + version);
    const url = 'https://artifactory.expresslrs.org/ExpressLRS/' + rel.sha + '/firmware.zip';
    const path = 'firmware/' + variant + '/' + model.fw + '/firmware.bin';
    status.textContent = 'Loading JSZip…';
    await ensureJSZip();
    status.textContent = 'Downloading ' + version + ' zip (~25 MB)…';
    const zipBlob = await (await fetch(url, {mode:'cors'})).blob();
    status.textContent = 'Unpacking ' + path + '…';
    const zip = await JSZip.loadAsync(zipBlob);
    const entry = zip.file(path);
    if (!entry) throw new Error('not in zip: ' + path);
    const fwBlob = await entry.async('blob');
    status.textContent = 'Uploading ' + (fwBlob.size/1024).toFixed(1) + ' KB…';
    const fd = new FormData();
    fd.append('firmware', fwBlob, 'firmware.bin');
    const txt = await postForm('/api/flash/upload', fd);
    _fwCached = { name: version + '/' + model.fw + '/' + variant, size: fwBlob.size };
    document.getElementById('fwCached').textContent = _fwCached.name + ' (' + (_fwCached.size/1024).toFixed(1) + ' KB)';
    status.textContent = '✓ ' + version + ' ready: ' + txt;
    status.style.color = '#0f0';
    fwPathUpdate();
  } catch (e) {
    status.textContent = '✗ ' + (e.message || e);
    status.style.color = '#f66';
  } finally {
    btn.disabled = false; btn.textContent = '⬇ Download + cache in PSRAM';
  }
}
let _fwLocalFile = null;
function fwUploadSelect() {
  _fwLocalFile = document.getElementById('fwUploadFile').files[0];
  document.getElementById('fwDoUploadBtn').disabled = !_fwLocalFile;
}
async function fwDoUpload() {
  if (!_fwLocalFile) return;
  const status = document.getElementById('fwResult');
  const btn = document.getElementById('fwDoUploadBtn');
  btn.disabled = true;
  try {
    status.textContent = 'Uploading ' + (_fwLocalFile.size/1024).toFixed(1) + ' KB…';
    const fd = new FormData();
    fd.append('firmware', _fwLocalFile);
    const txt = await postForm('/api/flash/upload', fd);
    _fwCached = { name: 'local:' + _fwLocalFile.name, size: _fwLocalFile.size };
    document.getElementById('fwCached').textContent = _fwCached.name + ' (' + (_fwCached.size/1024).toFixed(1) + ' KB)';
    status.textContent = '✓ ' + txt;
    status.style.color = '#0f0';
    fwPathUpdate();
  } catch (e) {
    status.textContent = '✗ ' + (e.message || e);
    status.style.color = '#f66';
  } finally {
    btn.disabled = false;
  }
}
async function fwFlash() {
  const status = document.getElementById('fwResult');
  const stage  = document.getElementById('fwStage');
  const bar    = document.getElementById('fwBar');
  const btn    = document.getElementById('fwFlashBtn');
  const target = document.getElementById('fwTarget').value;
  const bootAfter = document.getElementById('fwBootAfter').checked;
  if (!_fwCached) { alert('Cache firmware first'); return; }
  // Path selection from current RX mode.
  const via = (target !== '0x0' && (_rxMode === 'app' || _rxMode === 'stub')) ? 'stub' : '';
  const pathMsg = via ? 'via in-app stub (no BOOT required)' : 'via ROM DFU (RX must be in DFU)';
  const slotName = (target === '0x10000') ? 'app0' : (target === '0x1f0000') ? 'app1' : null;
  const bootMsg = (bootAfter && slotName) ? (' + flip OTADATA to ' + slotName) : '';
  if (!(await confirmModal('Flash ' + _fwCached.name + ' to ' + target + ' ' + pathMsg + bootMsg, {danger:true, okLabel:'Flash'}))) return;
  btn.disabled = true;
  status.textContent = '';
  let flashOk = false;
  try {
    const fd = new FormData();
    fd.append('offset', target);
    if (via) fd.append('via', via);
    fd.append('stay', bootAfter && slotName ? '1' : '0');  // keep DFU open if we'll flip OTADATA next
    const reply = await postForm('/api/flash/start', fd);
    status.textContent = reply;
    // Show inverse "flash in progress" banner on rcv-live (so if user
    // switches sections they see Start is gated). Hidden on completion.
    const liveBanner = document.getElementById('rcvFlashBanner');
    const startBtn   = document.getElementById('crsfStartBtn');
    if (liveBanner) liveBanner.style.display = '';
    if (startBtn)   { startBtn.disabled = true; startBtn._flashGated = true; }
    // Poll /api/flash/status with elapsed + ETA in stage label.
    const t0 = Date.now();
    let lastPct = 0;
    for (let i = 0; i < 120; i++) {
      await new Promise(r => setTimeout(r, 3000));
      const s = await getJson('/api/flash/status');
      const pct = s.progress || 0;
      const elapsed = Math.round((Date.now() - t0) / 1000);
      // ETA: linear extrapolation from current progress. Skip first ~5s
      // (progress ramps non-linearly during erase) to avoid wild numbers.
      let etaTxt = '';
      if (pct >= 5 && pct < 100 && elapsed > 5) {
        const total = elapsed * 100 / pct;
        const remaining = Math.max(0, Math.round(total - elapsed));
        etaTxt = `  · elapsed ${elapsed}s · ETA ~${remaining}s`;
      } else if (elapsed > 5) {
        etaTxt = `  · elapsed ${elapsed}s`;
      }
      stage.textContent = (s.stage || '-') + etaTxt;
      bar.style.width = pct + '%';
      lastPct = pct;
      if (!s.in_progress && s.lastResult) {
        flashOk = s.lastResult.startsWith('OK');
        status.textContent = (flashOk ? '✓ ' : '✗ ') + s.lastResult
                           + `  (total ${elapsed}s)`;
        status.style.color = flashOk ? '#0f0' : '#f66';
        break;
      }
    }
    // Clear inverse banner when flash ends (success OR fail OR timeout).
    if (liveBanner) liveBanner.style.display = 'none';
    if (startBtn) { startBtn._flashGated = false; startBtn.disabled = false; }

    // Auto-flip OTADATA to the newly-flashed slot if requested + successful.
    if (flashOk && bootAfter && slotName) {
      try {
        const fd2 = new FormData(); fd2.append('slot', slotName === 'app0' ? '0' : '1');
        const otaReply = await postForm('/api/otadata/select', fd2);
        status.textContent += '\n✓ ' + otaReply;
      } catch (e) {
        status.textContent += '\n✗ OTADATA flip failed: ' + (e.message || e);
      }
    }
    // Cached buffer gets freed server-side on flash completion — reflect that.
    _fwCached = null;
    document.getElementById('fwCached').textContent = '—';
    fwPathUpdate();
    // Auto-probe once RX has time to reboot.
    if (flashOk) setTimeout(() => { try { rxProbeMode(); } catch(_) {} }, 4500);
  } catch (e) {
    status.textContent = '✗ ' + (e.message || e);
    status.style.color = '#f66';
  } finally {
    btn.disabled = false;
  }
}

// ===== RX Configuration (LUA params) =====
let _rxCfgData = null;
const CRSF_TYPE_NAMES = { 0:'uint8', 1:'int8', 2:'uint16', 3:'int16', 4:'uint32', 5:'int32',
                          8:'float', 9:'select', 10:'string', 11:'folder', 12:'info', 13:'command' };
async function rxCfgLoad() {
  const btn = document.getElementById('rxCfgLoadBtn');
  const status = document.getElementById('rxCfgStatus');
  btn.disabled = true; btn.textContent = '⏳ Reading params (~2s)…';
  status.textContent = '';
  try {
    const d = await getJson('/api/elrs/params');
    _rxCfgData = d;
    status.textContent = 'Device "' + (d.device?.name || '?') + '" — ' + (d.params?.length || 0) + ' / ' + (d.device?.field_count || 0) + ' params';
    rxCfgRender();
  } catch (e) {
    status.textContent = '✗ ' + (e.message || e);
    status.style.color = '#f66';
  } finally {
    btn.disabled = false; btn.textContent = '🔄 Load parameters';
  }
}
function rxCfgRender() {
  if (!_rxCfgData) return;
  const showHidden = document.getElementById('rxCfgShowHidden').checked;
  const body = document.getElementById('rxCfgBody');
  const tbl  = document.getElementById('rxCfgTable');
  body.innerHTML = '';
  for (const p of _rxCfgData.params) {
    if (p.hidden && !showHidden) continue;
    const tr = document.createElement('tr');
    tr.style.borderBottom = '1px solid var(--border-soft)';
    tr.style.color = p.hidden ? 'var(--text-dim)' : '';
    let valueCell = '—';
    const tn = CRSF_TYPE_NAMES[p.type] || ('t' + p.type);
    // ✎ glyph signals "click to edit" for the inline inputs — without it,
    // a plain <input> in a tight row reads as a static label to most users.
    const editIcon = '<span class="pencil" title="Click to edit">✎</span>';
    if (p.type === 9) {
      // TEXT_SELECTION — dropdown
      const opts = (p.options || '').split(';');
      valueCell = editIcon + '<select onchange="rxCfgWrite(' + p.id + ',' + p.type + ',this.value)" title="Click to edit">' +
        opts.map((o, i) => '<option value="' + i + '"' + (i === p.value ? ' selected' : '') + '>' + o + '</option>').join('') +
      '</select>';
    } else if (p.type === 0 || p.type === 1 || p.type === 2 || p.type === 3) {
      // Numeric — inline input
      valueCell = editIcon + '<input type="number" value="' + p.value + '" min="' + p.min + '" max="' + p.max +
        '" onchange="rxCfgWrite(' + p.id + ',' + p.type + ',this.value)" title="Click to edit (range below)" style="width:80px;padding:2px">';
    } else if (p.type === 10) {
      // STRING
      valueCell = editIcon + '<input type="text" value="' + (p.value || '') + '" onchange="rxCfgWrite(' + p.id + ',' + p.type + ',this.value)" title="Click to edit" style="width:160px;padding:2px">';
    } else if (p.type === 11) {
      valueCell = '<em style="color:var(--text-dim)">▸ folder</em>';
    } else if (p.type === 12) {
      valueCell = '<em style="color:var(--text-dim)" title="Read-only INFO">' + (p.value || '') + '</em>';
    } else if (p.type === 13) {
      valueCell = '<button onclick="rxCfgWrite(' + p.id + ',' + p.type + ',1)" title="Run command on RX" style="padding:2px 8px">▶ Execute</button>';
    }
    const range = (p.min !== undefined) ? ('[' + p.min + '..' + p.max + '] def=' + p.default) : '';
    tr.innerHTML =
      '<td style="padding:3px">' + p.id + (p.hidden ? ' ¬' : '') + '</td>' +
      '<td>' + p.name + '</td>' +
      '<td>' + tn + '</td>' +
      '<td>' + valueCell + '</td>' +
      '<td>' + range + '</td>';
    body.appendChild(tr);
  }
  tbl.style.display = body.childElementCount ? 'table' : 'none';
  const ph = document.getElementById('rxCfgPlaceholder');
  if (ph) ph.style.display = body.childElementCount ? 'none' : '';
}
async function rxCfgWrite(id, type, value) {
  try {
    const fd = new FormData();
    fd.append('id', id); fd.append('type', type); fd.append('value', value);
    const txt = await postForm('/api/elrs/params/write', fd);
    const s = document.getElementById('rxCfgStatus');
    s.textContent = '✓ ' + txt;
    s.style.color = '#0f0';
    // Re-read after short delay to verify
    setTimeout(rxCfgLoad, 500);
  } catch (e) {
    alert('Write failed: ' + (e.message || e));
  }
}

// ===== Controls =====
function ctrlLog(msg, err) {
  const el = document.getElementById('ctrlMsg');
  const ts = new Date().toLocaleTimeString();
  el.textContent = ts + '  ' + (err ? '✗ ' : '✓ ') + msg + '\n' + (el.textContent || '');
  el.style.color = err ? '#f66' : '#0f0';
}
function ctrlAutoProbe(delayMs) {
  setTimeout(() => { try { rxProbeMode(); } catch(_) {} }, delayMs);
}
async function ctrlBind()  { try { ctrlLog(await postForm('/api/elrs/bind')); ctrlAutoProbe(2000); }
                            catch (e) { ctrlLog(e.message || e, true); } }
async function ctrlStub()  { try { ctrlLog(await postForm('/api/crsf/reboot_to_bl')); ctrlAutoProbe(1500); }
                            catch (e) { ctrlLog(e.message || e, true); } }
async function ctrlExitDfu() { try { ctrlLog(await postForm('/api/flash/exit_dfu')); ctrlAutoProbe(3000); }
                               catch (e) { ctrlLog(e.message || e, true); } }
async function ctrlBoot(slot) {
  try {
    const fd = new FormData(); fd.append('slot', String(slot));
    const r = await postForm('/api/otadata/select', fd);
    ctrlLog(r);
    ctrlLog('RX should reboot into app' + slot + ' in ~2s. Probing in 4s…');
    setTimeout(() => { try { rxProbeMode(); } catch(_) {} }, 4000);
  } catch (e) { ctrlLog(e.message || e, true); }
}

// ===== Receiver Overview (unified one-session identity) =====
async function rxScan() {
  const btn = document.getElementById('rxScanBtn');
  const err = document.getElementById('rxScanErr');
  const ov  = document.getElementById('rxOv');
  btn.disabled = true; err.textContent = ''; btn.textContent = '⏳ Scanning…';
  try {
    const txt = await postForm('/api/elrs/receiver_info');
    const d = JSON.parse(txt);
    document.getElementById('rxChip').textContent =
      (d.chip.name || 'unknown') + '  (magic ' + (d.chip.magic_hex || '-') + ')';
    document.getElementById('rxMac').textContent = d.chip.mac_ok ? d.chip.mac : '(not read)';
    document.getElementById('rxActive').textContent =
      d.otadata.active_slot >= 0 ? ('app' + d.otadata.active_slot) : 'none (both blank)';
    document.getElementById('rxMaxSeq').textContent =
      d.otadata.max_seq + (d.otadata.ok ? '' : ' (read failed)');

    const slotsDiv = document.getElementById('rxSlots');
    slotsDiv.innerHTML = '';
    for (let i = 0; i < 2; i++) {
      const s = d.slots[i];
      const col = document.createElement('div');
      col.style.cssText = 'flex:1 1 220px;background:var(--card-bg2);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:12px';
      const badge = s.active
        ? '<span style="background:#0a3;color:#fff;padding:1px 6px;border-radius:3px">ACTIVE</span>'
        : (s.present ? '<span style="background:#333;color:#ccc;padding:1px 6px;border-radius:3px">inactive</span>'
                     : '<span style="background:#633;color:#fcc;padding:1px 6px;border-radius:3px">empty</span>');
      let body = '<div style="font-weight:bold;margin-bottom:4px">app' + i + ' @ ' + s.offset + ' &nbsp; ' + badge + '</div>';
      if (s.present) {
        body += '<div><b>target:</b> <code style="color:var(--accent)">' + (s.target || '-') + '</code></div>';
        body += '<div><b>version/lua:</b> ' + (s.version_or_lua || '-') + '</div>';
        body += '<div><b>git:</b> <code>' + (s.git || '-') + '</code></div>';
        body += '<div><b>product:</b> ' + (s.product || '-') + '</div>';
        body += '<div><b>entry:</b> <code>' + s.entry + '</code></div>';
      } else {
        body += '<div style="color:var(--text-dim);font-style:italic">slot blank or corrupt (first byte != 0xE9)</div>';
      }
      body += '<div style="margin-top:6px;display:flex;gap:4px;flex-wrap:wrap">';
      if (!s.active && s.present) {
        body += '<button onclick="otadataSelect(' + i + ')" style="font-size:11px;padding:3px 6px">Boot app' + i + '</button>';
      }
      body += '<button onclick="rxBackupSlot(' + i + ')" style="font-size:11px;padding:3px 6px">Backup</button>';
      body += '<button onclick="rxFlashToSlot(' + i + ')" style="font-size:11px;padding:3px 6px;background:#0a3" title="ROM DFU path (manual BOOT+power-cycle required)">Flash (DFU)</button>';
      body += '<button onclick="rxFlashStub(' + i + ')" style="font-size:11px;padding:3px 6px;background:#06c" title="Auto-flash via in-app ELRS stub @420000 — RX must be in app (not DFU)">⚡ Stub-flash</button>';
      body += '<button class="danger" onclick="rxEraseSlot(' + i + ')" style="font-size:11px;padding:3px 6px">Erase</button>';
      body += '</div>';
      col.innerHTML = body;
      slotsDiv.appendChild(col);
    }
    ov.style.display = 'block';
    rxApplyModeGate();  // re-gate after slot buttons rendered
  } catch (e) {
    err.textContent = 'Scan failed: ' + (e.message || e);
  } finally {
    btn.disabled = false; btn.textContent = '🔍 Scan receiver';
  }
}
function rxBackupSlot(slot) {
  const off = slot === 0 ? '0x10000' : '0x1f0000';
  document.getElementById('dumpOffset').value = off;
  document.getElementById('dumpSize').value = '0x200000';
  alert('Dump offset/size prefilled. Open Receiver tab → Advanced → "Dump Receiver Firmware" and press Dump.');
  showTab('receiver');
}
function rxFlashToSlot(slot) {
  const radio = document.querySelector('input[name="slotSel"][value="app' + slot + '"]');
  if (radio) { radio.checked = true; if (typeof slotOnSlotChange === 'function') slotOnSlotChange(); }
  alert('Slot app' + slot + ' selected. Scroll to "Slot-targeted flash" — upload firmware.bin and hit Erase+Flash.');
}

// ===== Stub-flash (auto, no BOOT+power-cycle required) =====
// Sends CRSF 'bl' frame to running RX then flashes via the in-app esptool
// stub @ 420000. Only works if RX is currently running the app. If RX is in
// real ROM DFU (physical BOOT), use the normal Flash (DFU) path instead.
async function rxFlashStub(slot) {
  const r = document.getElementById('elrsResolved');
  const fwInfo = r && r.textContent && r.textContent !== '-'
    ? ' (current catalog selection: ' + r.textContent + ')'
    : '';
  if (!(await confirmModal(
      'Auto-flash via in-app stub to app' + slot + '?\n\n' +
      '1. Receiver must be running the app (LED on, vanilla ELRS or MILELRS).\n' +
      '2. Plate will send CRSF \'bl\' frame → RX enters stub @ 420000 on same pins.\n' +
      '3. Flash proceeds without physical BOOT + power-cycle.\n\n' +
      'Requires firmware already uploaded to plate PSRAM (use Catalog Fetch or the file-upload card below).' + fwInfo,
      {danger:true, okLabel:'Stub-flash'}
  ))) return;
  const fd = new FormData();
  fd.append('offset', slot === 0 ? '0x10000' : '0x1f0000');
  fd.append('via', 'stub');
  // Stub cannot touch bootloader; stay flag irrelevant (FLASH_END reboots)
  fd.append('stay', '0');
  try {
    const txt = await postForm('/api/flash/start', fd);
    alert('Stub-flash queued: ' + txt + '\n\nWatch /api/flash/status or the main flash progress widget. When done, power-cycle is NOT required — RX auto-reboots into new app.');
  } catch (e) {
    alert('Stub-flash failed: ' + (e.message || e));
  }
}
async function rxEraseSlot(slot) {
  if (!(await confirmModal('Erase app' + slot + ' partition (1.88 MB)? This is irreversible.', {danger:true, okLabel:'Erase'}))) return;
  const off = slot === 0 ? '0x10000' : '0x1f0000';
  const fd = new FormData();
  fd.append('offset', off);
  fd.append('size', '0x1e0000');
  fd.append('chunk', '0x40000');
  try {
    const r = await postForm('/api/flash/erase_partition', fd);
    alert('Erase done: ' + r);
    rxScan();
  } catch (e) {
    alert('Erase failed: ' + (e.message || e));
  }
}

// ===== Firmware Catalog =====
const ELRS_RELEASES = [
  {version:'4.0.0', date:'2026-02-06', sha:'ed9fc3e637207e8d656ffe9b1b3e8eef418573c6', note:'latest (LittleFS)'},
  {version:'3.6.3', date:'2026-01-21', sha:'288efe1acf223e479f81349d68dda5505135301a', note:'last 3.x'},
  {version:'3.5.3', date:'2024-11-29', sha:'40555e141efb0c93ea8d075ec47a27592355f924', note:'known-good on BAYCK C3 Dual'},
];

// Curated list of RX/TX models → {chip, radio, firmware variant, product_name}.
// Sourced from ELRS targets.json (master). Used to feed the catalog selector
// so users pick "BAYCK C3 Dual" rather than raw "Unified_ESP32C3_LR1121_RX".
const ELRS_MODELS = [
  // --- Bayck RC ---
  {id:'bayck.rx_dual.dualc3', name:'BAYCK RC C3 Dual Band 100mW Gemini RX (our RX)', chip:'esp32-c3', fw:'Unified_ESP32C3_LR1121_RX', role:'RX', note:'= our test receiver'},
  {id:'bayck.rx_dual.dual',   name:'BAYCK 900/2400 Dual Band Gemini RX (ESP32)',      chip:'esp32',    fw:'Unified_ESP32_LR1121_RX',   role:'RX'},
  {id:'bayck.rx_dual.single', name:'BAYCK C3 Dual Band Nano RX',                      chip:'esp32-c3', fw:'Unified_ESP32C3_LR1121_RX', role:'RX'},
  {id:'bayck.tx_dual.nano',         name:'BAYCK Dual Band 1W Nano TX',         chip:'esp32', fw:'Unified_ESP32_LR1121_TX', role:'TX'},
  {id:'bayck.tx_dual.nano_gemini',  name:'BAYCK Dual Band 1W Nano Gemini TX',  chip:'esp32', fw:'Unified_ESP32_LR1121_TX', role:'TX'},
  {id:'bayck.tx_dual.micro_gemini', name:'BAYCK Dual Band 1W Micro Gemini TX', chip:'esp32', fw:'Unified_ESP32_LR1121_TX', role:'TX'},
  // --- Generic C3 + LR1121 (rrd2, rr2) ---
  {id:'generic.c3.lr1121.diversity', name:'Generic C3 LR1121 True Diversity RX', chip:'esp32-c3', fw:'Unified_ESP32C3_LR1121_RX', role:'RX'},
  {id:'generic.c3.lr1121.single',    name:'Generic C3 LR1121 RX',                chip:'esp32-c3', fw:'Unified_ESP32C3_LR1121_RX', role:'RX'},
  // --- Popular 2.4 GHz only ESP32-C3 ---
  {id:'generic.c3.sx1280.rx',   name:'Generic C3 SX1280 2.4 GHz RX', chip:'esp32-c3', fw:'Unified_ESP32C3_2400_RX', role:'RX'},
  {id:'matek.2400_rx.r24d',     name:'Matek R24-D 2.4 GHz RX (ESP8285)', chip:'esp8285', fw:'Unified_ESP8285_2400_RX', role:'RX'},
  // --- ESP32 + SX1280 TX ---
  {id:'generic.esp32.2400_tx',  name:'Generic ESP32 2.4 GHz TX', chip:'esp32', fw:'Unified_ESP32_2400_TX', role:'TX'},
  // --- ESP8285 900 MHz RX ---
  {id:'generic.esp8285.900_rx', name:'Generic ESP8285 900 MHz RX', chip:'esp8285', fw:'Unified_ESP8285_900_RX', role:'RX'},
];
function renderCatalog() {
  const host = document.getElementById('elrsCatalog');
  if (!host) return;
  host.innerHTML = '';
  for (const r of ELRS_RELEASES) {
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;align-items:center;gap:6px;padding:4px;border:1px solid var(--border-soft);border-radius:4px';
    row.innerHTML =
      '<div style="flex:1"><b>' + r.version + '</b> · ' + r.date +
      ' <span style="color:var(--text-dim);font-size:11px">' + r.note + '</span></div>' +
      '<button onclick="catFetch(\'' + r.version + '\', \'' + r.sha + '\')" style="font-size:12px">Fetch</button>' +
      '<a href="https://artifactory.expresslrs.org/ExpressLRS/' + r.sha + '/firmware.zip" target="_blank" style="font-size:11px;color:var(--accent)">zip</a>';
    host.appendChild(row);
  }
  // Populate model selector, preselect BAYCK C3 Dual (our known RX).
  const ms = document.getElementById('elrsModel');
  if (ms && !ms.options.length) {
    for (const m of ELRS_MODELS) {
      const opt = document.createElement('option');
      opt.value = m.id;
      opt.textContent = m.name + '  [' + m.role + ' · ' + m.chip + ']';
      opt.dataset.fw = m.fw;
      ms.appendChild(opt);
    }
    ms.value = 'bayck.rx_dual.dualc3';
    ms.onchange = catUpdateResolved;
    document.getElementById('elrsVariant').onchange = catUpdateResolved;
    catUpdateResolved();
  }
}
function catModel() {
  const id = document.getElementById('elrsModel').value;
  return ELRS_MODELS.find(m => m.id === id);
}
function catUpdateResolved() {
  const m = catModel();
  const variant = document.getElementById('elrsVariant').value;
  if (!m) return;
  document.getElementById('elrsResolved').textContent =
    'firmware/' + variant + '/' + m.fw + '/firmware.bin';
}
async function ensureJSZip() {
  if (window.JSZip) return;
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = 'https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js';
    s.onload = resolve;
    s.onerror = () => reject(new Error('failed to load JSZip CDN'));
    document.head.appendChild(s);
  });
}
async function catFetch(version, sha) {
  const status = document.getElementById('catStatus');
  const variant = document.getElementById('elrsVariant').value;
  const target  = document.getElementById('elrsTarget').value;
  const url = 'https://artifactory.expresslrs.org/ExpressLRS/' + sha + '/firmware.zip';
  const path = 'firmware/' + variant + '/' + target + '/firmware.bin';
  try {
    status.textContent = 'Loading JSZip…';
    await ensureJSZip();
    status.textContent = 'Downloading ' + version + ' zip (~25 MB) from artifactory…';
    const zipBlob = await (await fetch(url, {mode:'cors'})).blob();
    status.textContent = 'Unpacking ' + path + ' from zip…';
    const zip = await JSZip.loadAsync(zipBlob);
    const entry = zip.file(path);
    if (!entry) throw new Error('not in zip: ' + path + '. Target might differ between versions — check zip contents');
    const fwBlob = await entry.async('blob');
    status.textContent = 'Uploading ' + (fwBlob.size/1024).toFixed(1) + ' KB to plate buffer…';
    const fd = new FormData();
    fd.append('firmware', fwBlob, 'firmware.bin');
    const txt = await postForm('/api/flash/upload', fd);
    status.textContent = '✓ ' + version + ' ready in PSRAM: ' + txt + '\nNow scroll to "Slot-targeted flash" and Flash to app0 or app1.';
    status.style.color = '#0f0';
  } catch (e) {
    status.textContent = '✗ ' + (e.message || e);
    status.style.color = '#f66';
  }
}

// ===== Chip detection =====
async function detectChip() {
  const btn = document.getElementById('chipDetectBtn');
  const setRes = (msg, err) => {
    const el = document.getElementById('chipResult');
    el.style.color = err ? '#f66' : '#0f0';
    el.textContent = (new Date().toLocaleTimeString()) + '  ' + msg;
  };
  btn.disabled = true;
  document.getElementById('chipName').textContent = 'detecting...';
  document.getElementById('chipMac').textContent = '-';
  document.getElementById('chipMagic').textContent = '-';
  try {
    const txt = await postForm('/api/elrs/chip_info');
    const j = JSON.parse(txt);
    document.getElementById('chipName').textContent = j.chip || '-';
    document.getElementById('chipMac').textContent = j.mac_ok ? j.mac : '(not supported for this chip)';
    document.getElementById('chipMagic').textContent = j.magic_hex || '-';
    setRes('detect OK: ' + (j.chip || 'unknown'));
  } catch (e) {
    document.getElementById('chipName').textContent = 'FAIL';
    setRes(e.message, true);
  } finally {
    btn.disabled = false;
  }
}

let selectedFile = null;
function onFwSelect() {
  selectedFile = document.getElementById('fwFile').files[0];
  if (selectedFile) {
    document.getElementById('fwSize').textContent = (selectedFile.size/1024).toFixed(1) + ' KB';
    document.getElementById('uploadBtn').disabled = false;
  }
}
async function uploadFw() {
  if (!selectedFile) return;
  const fd = new FormData();
  fd.append('firmware', selectedFile);
  const stage = document.getElementById('flashStage');
  const result = document.getElementById('flashResult');
  stage.textContent = 'Uploading...';
  document.getElementById('uploadBtn').disabled = true;
  document.getElementById('flashBtn').disabled = true;
  try {
    const t = await postForm('/api/flash/upload', fd);
    stage.textContent = 'Uploaded';
    result.style.color = '#0f0';
    result.textContent = t;
    document.getElementById('flashBtn').disabled = false;
  } catch (e) {
    stage.textContent = 'Upload error';
    result.style.color = '#f66';
    result.textContent = e.message;
  } finally {
    document.getElementById('uploadBtn').disabled = false;
  }
}
let _flashPoll = null;
async function startFlash() {
  if (!(await confirmModal('Убедись что приёмник в DFU режиме!\nПродолжить прошивку?', {danger:true, okLabel:'Прошивать'}))) return;
  const btn = document.getElementById('flashBtn');
  const result = document.getElementById('flashResult');
  btn.disabled = true;
  result.style.color = '#0f0';
  result.textContent = '';
  try {
    const t = await postForm('/api/flash/start');
    result.textContent = t;
    if (!_flashPoll) _flashPoll = setInterval(flashPoll, 1000);
  } catch (e) {
    document.getElementById('flashStage').textContent = 'start error';
    result.style.color = '#f66';
    result.textContent = e.message;
    btn.disabled = false;
  }
}
function flashPoll() {
  fetch('/api/flash/status').then(r => r.ok ? r.json() : null).then(j => {
    if (!j) return;
    const stage = j.stage || (j.in_progress ? 'flashing' : 'idle');
    const pct = j.progress_pct || 0;
    document.getElementById('flashStage').textContent =
      j.in_progress ? (stage + ' (' + pct + '%)') : stage;
    document.getElementById('flashBar').style.width = pct + '%';
    if (!j.in_progress && !j.requested) {
      if (_flashPoll) { clearInterval(_flashPoll); _flashPoll = null; }
      if (j.lastResult) {
        const lr = j.lastResult;
        const result = document.getElementById('flashResult');
        // "OK (verified …)" = success; anything else = failure
        const isOk = lr.startsWith('OK') || /verified/i.test(lr);
        result.style.color = isOk ? '#0f0' : '#f66';
        result.textContent = 'Result: ' + lr;
      }
      // Re-enable flash button iff a firmware buffer is still loaded
      document.getElementById('flashBtn').disabled = (j.fw_size === 0);
    }
  }).catch(()=>{ /* transient WiFi glitch — keep polling */ });
}
function clearFw() {
  postForm('/api/flash/clear').then(()=>{
    document.getElementById('fwFile').value = '';
    document.getElementById('fwSize').textContent = '-';
    document.getElementById('flashStage').textContent = 'idle';
    document.getElementById('flashBar').style.width = '0%';
    document.getElementById('flashResult').textContent = '';
    document.getElementById('uploadBtn').disabled = true;
    document.getElementById('flashBtn').disabled = true;
    selectedFile = null;
  }).catch(e => {
    document.getElementById('flashResult').style.color = '#f66';
    document.getElementById('flashResult').textContent = 'clear failed: ' + e.message;
  });
}

// === Receiver firmware DUMP ===
let _dumpPoll = null;
async function dumpStart() {
  const offset = document.getElementById('dumpOffset').value;
  const size   = document.getElementById('dumpSize').value;
  const fd = new FormData();
  fd.append('offset', offset);
  fd.append('size', size);
  document.getElementById('dumpError').textContent = '';
  document.getElementById('dumpStage').textContent = 'starting...';
  document.getElementById('dumpStartBtn').disabled = true;
  document.getElementById('dumpDownloadBtn').disabled = true;
  try {
    const t = await postForm('/api/flash/dump/start', fd);
    document.getElementById('dumpStage').textContent = t;
    if (!_dumpPoll) _dumpPoll = setInterval(dumpPoll, 1000);
  } catch (e) {
    document.getElementById('dumpError').textContent = e.message;
    document.getElementById('dumpStage').textContent = 'start failed';
    document.getElementById('dumpStartBtn').disabled = false;
  }
}
function dumpPoll() {
  fetch('/api/flash/dump/status').then(r => r.ok ? r.json() : null).then(j => {
    if (!j) return;
    document.getElementById('dumpStage').textContent = j.stage || (j.running ? 'reading' : 'idle');
    document.getElementById('dumpBar').style.width = (j.progress || 0) + '%';
    document.getElementById('dumpError').textContent = j.error || '';
    if (!j.running) {
      if (_dumpPoll) { clearInterval(_dumpPoll); _dumpPoll = null; }
      document.getElementById('dumpStartBtn').disabled = false;
      document.getElementById('dumpDownloadBtn').disabled = !j.ready;
    }
  }).catch(e => {
    document.getElementById('dumpError').textContent = 'poll error: ' + e.message;
  });
}
function dumpDownload() {
  window.location.href = '/api/flash/dump/download';
}
async function dumpClear() {
  try {
    await postForm('/api/flash/dump/clear');
    document.getElementById('dumpStage').textContent = 'cleared';
    document.getElementById('dumpBar').style.width = '0%';
    document.getElementById('dumpError').textContent = '';
    document.getElementById('dumpDownloadBtn').disabled = true;
  } catch (e) {
    document.getElementById('dumpError').textContent = 'clear failed: ' + e.message;
  }
}

// ===================================================================
// Slot-targeted flash (app0 / app1 / custom)
// ===================================================================
let _slotFile = null;
let _slotFlashPoll = null;

function slotOnFwSelect() {
  const f = document.getElementById('slotFwFile').files[0];
  if (!f) { _slotFile = null; return; }
  _slotFile = f;
  document.getElementById('slotFwSize').textContent = (f.size / 1024).toFixed(1) + ' KB';
  document.getElementById('slotUploadBtn').disabled = false;
  document.getElementById('slotFlashBtn').disabled = true;
}

function slotOnSlotChange() {
  const sel = document.querySelector('input[name="slotSel"]:checked').value;
  const customInp = document.getElementById('slotCustomOffset');
  if (sel === 'app0')   { customInp.disabled = true;  customInp.value = '0x10000';  }
  else if (sel === 'app1') { customInp.disabled = true;  customInp.value = '0x1F0000'; }
  else                   { customInp.disabled = false; }
}

function slotSelectedOffset() {
  const sel = document.querySelector('input[name="slotSel"]:checked').value;
  if (sel === 'app0') return RX_PARTITIONS.app0.offset;
  if (sel === 'app1') return RX_PARTITIONS.app1.offset;
  const raw = document.getElementById('slotCustomOffset').value.trim();
  return parseInt(raw, raw.toLowerCase().startsWith('0x') ? 16 : 10);
}

function slotPartitionSize() {
  const sel = document.querySelector('input[name="slotSel"]:checked').value;
  if (sel === 'app0' || sel === 'app1') return RX_PARTITIONS[sel].size;
  return RX_PARTITIONS.app0.size;  // default for custom
}

function slotLog(msg, isError) {
  const el = document.getElementById('slotResult');
  el.style.color = isError ? '#f66' : '#0f0';
  el.textContent = (new Date().toLocaleTimeString()) + '  ' + msg + '\n' + el.textContent;
}

async function slotUpload() {
  if (!_slotFile) return;
  const fd = new FormData();
  fd.append('firmware', _slotFile);
  document.getElementById('slotStage').textContent = 'Uploading...';
  document.getElementById('slotUploadBtn').disabled = true;
  document.getElementById('slotFlashBtn').disabled = true;
  try {
    const t = await postForm('/api/flash/upload', fd);
    document.getElementById('slotStage').textContent = 'Uploaded';
    document.getElementById('slotFlashBtn').disabled = false;
    slotLog('upload OK: ' + t);
  } catch (e) {
    document.getElementById('slotStage').textContent = 'Upload error';
    slotLog('upload FAIL: ' + e.message, true);
  } finally {
    document.getElementById('slotUploadBtn').disabled = false;
  }
}

async function slotFlash() {
  const off = slotSelectedOffset();
  if (isNaN(off) || off < 0 || off >= 0x400000) {
    alert('Invalid offset'); return;
  }
  const eraseFirst = document.getElementById('slotEraseFirst').checked;
  const partSize = slotPartitionSize();
  const hexOff = '0x' + off.toString(16);
  if (!(await confirmModal('Прошить по offset ' + hexOff + (eraseFirst ? ' (с erase ' + (partSize/1024/1024).toFixed(2) + ' MB)' : '') + '?\nRX должен быть в DFU!', {danger:true, okLabel:'Прошивать'}))) return;

  document.getElementById('slotFlashBtn').disabled = true;
  document.getElementById('slotBar').style.width = '0%';

  try {
    if (eraseFirst) {
      document.getElementById('slotStage').textContent = 'Erasing partition...';
      slotLog('erase_region offset=' + hexOff + ' size=0x' + partSize.toString(16));
      const fdE = new FormData();
      fdE.append('offset', hexOff);
      fdE.append('size', '0x' + partSize.toString(16));
      const r = await fetch('/api/flash/erase_region', {method:'POST', body: fdE}).catch(e => { throw e; });
      const t = await r.text();
      if (!r.ok) throw new Error('erase failed: ' + t);
      slotLog('erase OK: ' + t);
    }

    document.getElementById('slotStage').textContent = 'Flashing @ ' + hexOff + '...';
    const fdF = new FormData();
    fdF.append('offset', hexOff);
    const rF = await fetch('/api/flash/start', {method:'POST', body: fdF}).catch(e => { throw e; });
    const tF = await rF.text();
    if (!rF.ok) throw new Error('flash start failed: ' + tF);
    slotLog('flash started: ' + tF);
    if (!_slotFlashPoll) _slotFlashPoll = setInterval(slotFlashPollFn, 1000);
  } catch (e) {
    document.getElementById('slotStage').textContent = 'ERROR';
    slotLog('FAIL: ' + e.message, true);
    document.getElementById('slotFlashBtn').disabled = false;
  }
}

function slotFlashPollFn() {
  fetch('/api/flash/status').then(r => r.ok ? r.json() : null).then(j => {
    if (!j) return;
    const stage = j.stage || (j.in_progress ? 'flashing' : 'idle');
    document.getElementById('slotStage').textContent =
      j.in_progress ? (stage + ' (' + (j.progress_pct || 0) + '%)') : stage;
    document.getElementById('slotBar').style.width = (j.progress_pct || 0) + '%';
    if (!j.in_progress && !j.requested) {
      if (_slotFlashPoll) { clearInterval(_slotFlashPoll); _slotFlashPoll = null; }
      document.getElementById('slotFlashBtn').disabled = false;
      if (j.lastResult) {
        // "OK …" / "verified" = success. Anything with FAIL/error = bad.
        // Plain `ok` substring match used to match "Gzip decompress failed".
        const lr = j.lastResult;
        const isOk = (lr.startsWith('OK') || /verified/i.test(lr)) && !/FAIL|ERROR/i.test(lr);
        slotLog((isOk ? 'done OK: ' : 'FAIL: ') + lr, !isOk);
        if (isOk) {
          slotLog('→ Теперь используй OTADATA card ниже чтобы пометить этот слот активным.');
          rxWizMark('flash', 'ok');
        } else {
          rxWizMark('flash', 'fail');
        }
      }
    }
  }).catch(e => {
    // Transient error — keep polling. Real failures surface via j.lastResult.
  });
}

// ===================================================================
// OTADATA / active-slot controls
// ===================================================================
function otadataLog(msg, isError) {
  const el = document.getElementById('otadataResult');
  el.style.color = isError ? '#f66' : '#0f0';
  el.textContent = (new Date().toLocaleTimeString()) + '  ' + msg + '\n' + el.textContent;
}

function otadataRefresh() {
  fetch('/api/otadata/status').then(r => {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(j => {
    document.getElementById('otaActiveSlot').textContent =
      (j.active_slot === 0 || j.active_slot === 1) ? ('app' + j.active_slot) : (j.active_slot ?? '-');
    document.getElementById('otaMaxSeq').textContent = (j.max_seq !== undefined) ? j.max_seq : '-';
    const secs = j.sectors || [];
    document.getElementById('otaSec0').textContent = secs[0] ? JSON.stringify(secs[0]) : '-';
    document.getElementById('otaSec1').textContent = secs[1] ? JSON.stringify(secs[1]) : '-';
    rxWizMark('otadata', 'ok');
  }).catch(e => {
    document.getElementById('otaActiveSlot').textContent = '(RX не в DFU?)';
    otadataLog('status FAIL: ' + e, true);
  });
}

async function otadataSelect(slot) {
  if (!(await confirmModal('Пометить app' + slot + ' как активный? RX должен быть в DFU.'))) return;
  fetch('/api/otadata/select?slot=' + slot, {method:'POST'})
    .then(r => r.text().then(t => ({ok: r.ok, t})))
    .then(({ok, t}) => {
      otadataLog((ok ? 'select app' + slot + ' OK: ' : 'select FAIL: ') + t, !ok);
      if (ok) {
        otadataRefresh();
        rxWizMark('otadata_write', 'ok');
      }
    })
    .catch(e => otadataLog('select FAIL: ' + e, true));
}

function otadataEraseRegion() {
  // Type-to-confirm — same pattern as EraseApp1. confirm() is too easy to
  // dismiss accidentally; OTADATA loss can brick a fork-RX whose bootloader
  // doesn't fall back gracefully to app0.
  const typed = prompt(
    'Стереть OTADATA (@0xe000, 8 KB)?\n' +
    'Bootloader будет грузить app0 по умолчанию.\n' +
    'Стандартная ELRS пережёвывает; форки могут потерять конфиг.\n\n' +
    'Введи ERASE OTADATA чтобы подтвердить:');
  if (typed !== 'ERASE OTADATA') { otadataLog('erase OTADATA cancelled', false); return; }
  const fd = new FormData();
  fd.append('offset', '0xe000');
  fd.append('size', '0x2000');
  fetch('/api/flash/erase_region', {method:'POST', body: fd})
    .then(r => r.text().then(t => ({ok: r.ok, t})))
    .then(({ok, t}) => {
      otadataLog((ok ? 'erase OTADATA OK: ' : 'erase FAIL: ') + t, !ok);
      if (ok) otadataRefresh();
    })
    .catch(e => otadataLog('erase FAIL: ' + e, true));
}

function otadataEraseApp1() {
  const typed = prompt('Стереть app1 полностью (1.88 MB @ 0x1F0000)?\nЭто необратимо удалит MILELRS.\nВведи ERASE APP1 чтобы подтвердить:');
  if (typed !== 'ERASE APP1') { otadataLog('erase app1 cancelled', false); return; }
  const fd = new FormData();
  fd.append('offset', '0x' + RX_PARTITIONS.app1.offset.toString(16));
  fd.append('size',   '0x' + RX_PARTITIONS.app1.size.toString(16));
  otadataLog('erasing app1 (1.88 MB) — ~5 s...');
  fetch('/api/flash/erase_region', {method:'POST', body: fd})
    .then(r => r.text().then(t => ({ok: r.ok, t})))
    .then(({ok, t}) => {
      otadataLog((ok ? 'erase app1 OK: ' : 'erase app1 FAIL: ') + t, !ok);
    })
    .catch(e => otadataLog('erase app1 FAIL: ' + e, true));
}

// ===================================================================
// hardware.json helper — client-side download
// ===================================================================
function hwJsonPreviewRender() {
  const el = document.getElementById('hwJsonPreview');
  if (el && !el.textContent) el.textContent = JSON.stringify(RX_HARDWARE_JSON, null, 2);
}

function hwJsonDownload() {
  const blob = new Blob([JSON.stringify(RX_HARDWARE_JSON, null, 2)], {type: 'application/json'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'hardware.json';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ===================================================================
// RX vanilla-flip 5-step wizard
// ===================================================================
const RX_WIZ_STEPS = [
  { id: 'dfu',          label: 'Put RX in DFU (BOOT + power-cycle)',       action: rxWizStepDfu },
  { id: 'otadata',      label: 'Read OTADATA — confirm RX responds',       action: rxWizStepOtadata },
  { id: 'erase',        label: 'Erase app0 (1.88 MB @ 0x10000, ~5 s)',     action: rxWizStepErase },
  { id: 'flash',        label: 'Upload + flash vanilla ELRS @ 0x10000',    action: rxWizStepFlash },
  { id: 'otadata_write',label: 'Write OTADATA → boot app0, power-cycle',   action: rxWizStepOtadataWrite },
];
let _rxWizState = {};  // {stepId: 'pending'|'ok'|'fail'|'running'}

function rxWizMark(stepId, state) {
  _rxWizState[stepId] = state;
  rxWizRender();
}

function rxWizLog(msg, isError) {
  const el = document.getElementById('rxWizLog');
  if (!el) return;
  const line = (new Date().toLocaleTimeString()) + '  ' + msg;
  el.textContent = line + '\n' + el.textContent;
  if (isError) el.style.color = '#f66';
  else el.style.color = 'var(--text-dim)';
}

function rxWizRender() {
  const host = document.getElementById('rxWiz');
  if (!host) return;
  host.innerHTML = '';
  let prevOk = true;
  RX_WIZ_STEPS.forEach((step, i) => {
    const state = _rxWizState[step.id] || 'pending';
    const canRun = prevOk || state === 'fail';
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;align-items:center;gap:8px';
    let icon = '⚪';
    let color = 'var(--text-dim)';
    if (state === 'ok')      { icon = '✅'; color = '#0f0'; }
    else if (state === 'fail') { icon = '❌'; color = '#f66'; }
    else if (state === 'running') { icon = '⏳'; color = '#fa0'; }
    row.innerHTML = '<span style="font-size:14px">' + icon + '</span>' +
      '<span style="flex:1;color:' + color + '">[' + (i+1) + '/5] ' + step.label + '</span>';
    const btn = document.createElement('button');
    btn.textContent = state === 'ok' ? 'Redo' : (state === 'fail' ? 'Retry' : 'Run');
    btn.disabled = !canRun;
    if (state === 'ok') btn.classList.add('success');
    if (state === 'fail') btn.classList.add('danger');
    btn.onclick = () => { rxWizMark(step.id, 'running'); step.action(); };
    row.appendChild(btn);
    host.appendChild(row);
    if (state !== 'ok') prevOk = false;
  });
}

function rxWizReset() {
  _rxWizState = {};
  document.getElementById('rxWizLog').textContent = '';
  rxWizRender();
}

function rxWizStepDfu() {
  alert('1. Удержи кнопку BOOT на приёмнике.\n2. Подай питание (или переткни).\n3. Отпусти BOOT.\n\nКликни OK когда готово.');
  rxWizMark('dfu', 'ok');
  rxWizLog('DFU acknowledged by user');
}

function rxWizStepOtadata() {
  fetch('/api/otadata/status').then(r => {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(j => {
    rxWizLog('OTADATA responded: active_slot=' + j.active_slot + ' max_seq=' + j.max_seq);
    rxWizMark('otadata', 'ok');
    otadataRefresh();
  }).catch(e => {
    rxWizLog('OTADATA read FAIL: ' + e + ' — RX в DFU?', true);
    rxWizMark('otadata', 'fail');
  });
}

function rxWizStepErase() {
  const fd = new FormData();
  fd.append('offset', '0x' + RX_PARTITIONS.app0.offset.toString(16));
  fd.append('size',   '0x' + RX_PARTITIONS.app0.size.toString(16));
  rxWizLog('erasing app0 (1.88 MB) — ждать ~5 s...');
  fetch('/api/flash/erase_region', {method:'POST', body: fd})
    .then(r => r.text().then(t => ({ok: r.ok, t})))
    .then(({ok, t}) => {
      if (!ok) throw new Error(t);
      rxWizLog('erase app0 OK: ' + t);
      rxWizMark('erase', 'ok');
    })
    .catch(e => {
      rxWizLog('erase FAIL: ' + e, true);
      rxWizMark('erase', 'fail');
    });
}

function rxWizStepFlash() {
  // This step expects the user to have selected + uploaded the firmware via
  // the Slot-flash card. We just check buffer and trigger the flash.
  const sel = document.querySelector('input[name="slotSel"]:checked');
  if (!sel || sel.value !== 'app0') {
    alert('Выбери slot = app0 в карточке "Slot-targeted flash" и сделай Upload.');
    rxWizMark('flash', 'fail');
    return;
  }
  if (!_slotFile) {
    alert('Сначала выбери firmware.bin и нажми Upload в карточке Slot-targeted flash.');
    rxWizMark('flash', 'fail');
    return;
  }
  // Uncheck erase-first since we already erased in step 3
  document.getElementById('slotEraseFirst').checked = false;
  rxWizLog('delegating to slot-flash card: flashing @ 0x10000');
  slotFlash();  // slotFlashPollFn will call rxWizMark('flash', 'ok') on success
}

async function rxWizStepOtadataWrite() {
  if (!(await confirmModal('Записать OTADATA → app0 (POST /api/otadata/select?slot=0)?'))) {
    rxWizMark('otadata_write', 'fail');
    return;
  }
  fetch('/api/otadata/select?slot=0', {method:'POST'})
    .then(r => r.text().then(t => ({ok: r.ok, t})))
    .then(({ok, t}) => {
      if (!ok) throw new Error(t);
      rxWizLog('OTADATA → app0 OK: ' + t);
      rxWizLog('СНИМИ ПИТАНИЕ С RX, ПОДАЙ ЗАНОВО, ПРОВЕРЬ WiFi SSID.');
      rxWizMark('otadata_write', 'ok');
      otadataRefresh();
    })
    .catch(e => {
      rxWizLog('OTADATA write FAIL: ' + e, true);
      rxWizMark('otadata_write', 'fail');
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
async function clearWifi() {
  if (!(await confirmModal('Reset WiFi to AP mode?', {danger:true}))) return;
  fetch('/api/wifi/clear').then(r=>r.text()).then(t=>alert(t));
}

// ===== Setup wizard (Host/Bridge + device preset) =====
let _setupDevice = null;
let _setupRole = null;

function setupPickDevice(d) {
  _setupDevice = d;
  document.querySelectorAll('#setupDevice .action-card').forEach(b => {
    b.classList.toggle('active', b.dataset.device === d);
  });
  // motor has no bridge mode — hint user if they have bridge pre-selected.
  const hint = document.getElementById('setupBridgeHint');
  if (d === 'motor' && _setupRole === 'bridge') {
    hint.style.display = '';
    hint.textContent = '⚠ Motor/Servo не поддерживает BRIDGE (нет PC-тулов). Выбери HOST.';
    _setupRole = null;
    document.querySelectorAll('#setupRole .action-card').forEach(b => b.classList.remove('active'));
  } else {
    hint.style.display = 'none';
  }
  setupUpdateApplyBtn();
}
function setupPickRole(r) {
  if (_setupDevice === 'motor' && r === 'bridge') {
    document.getElementById('setupBridgeHint').style.display = '';
    document.getElementById('setupBridgeHint').textContent = '⚠ Motor/Servo BRIDGE не поддерживается.';
    return;
  }
  _setupRole = r;
  document.querySelectorAll('#setupRole .action-card').forEach(b => {
    b.classList.toggle('active', b.dataset.role === r);
  });
  setupUpdateApplyBtn();
}
function setupUpdateApplyBtn() {
  const btn = document.getElementById('setupApplyBtn');
  btn.disabled = !(_setupDevice && _setupRole);
}
function setupApply() {
  if (!_setupDevice || !_setupRole) return;
  const fd = new FormData();
  fd.append('device', _setupDevice);
  fd.append('role', _setupRole);
  document.getElementById('setupApplyMsg').textContent = 'Применяю...';
  fetch('/api/setup/apply', {method:'POST', body: fd})
    .then(r => r.json())
    .then(j => {
      if (!j.ok) {
        document.getElementById('setupApplyMsg').textContent = 'Ошибка: ' + JSON.stringify(j);
        return;
      }
      let msg = 'OK: USB=' + j.usb + ', Port B=' + j.port;
      if (j.reboot_needed) {
        msg += '. Перезагрузка для смены USB дескриптора...';
        document.getElementById('setupApplyMsg').textContent = msg;
        setTimeout(() => {
          fetch('/api/usb/reboot', {method:'POST'}).catch(()=>{});
          document.getElementById('setupApplyMsg').textContent =
            'Плата перезагружается. Refresh через 5-7с.';
        }, 400);
      } else {
        document.getElementById('setupApplyMsg').textContent = msg;
        setTimeout(setupRefresh, 500);
      }
    })
    .catch(e => {
      document.getElementById('setupApplyMsg').textContent = 'Ошибка: ' + e;
    });
}
function autodetectWiring(signal) {
  const el = document.getElementById('autodetectMsg');
  el.textContent = 'Ищу сигнал ' + signal + '... (до 3 секунд)';
  const fd = new FormData(); fd.append('signal', signal);
  fetch('/api/port/autodetect', {method:'POST', body: fd})
    .then(r => r.json())
    .then(j => {
      let msg = '';
      if (j.detected) {
        msg = '✓ ' + signal.toUpperCase() + ' найден';
        msg += j.swap_used ? ' (пины SWAPPED — сохранил в NVS)'
                           : ' (прямая раскладка, swap не нужен)';
        msg += `. Пины: TX=GP${j.tx_pin} RX=GP${j.rx_pin} SDA=GP${j.sda_pin} SCL=GP${j.scl_pin}`;
      } else {
        msg = '✗ ' + signal.toUpperCase() + ' не найден. Проверь GND/питание и тип сигнала.';
      }
      el.textContent = msg;
      setTimeout(setupRefresh, 300);
    })
    .catch(e => { el.textContent = 'Ошибка: ' + e; });
}

function setupRefresh() {
  fetch('/api/setup/status').then(r => r.json()).then(j => {
    const a = j.active, p = j.preferred;
    const fmt = (x) => (x.role === 'bridge' ? '💻 ' : '🖥️ ') +
                      (x.device === 'battery' ? '🔋 Battery' :
                       x.device === 'receiver' ? '📡 Receiver' :
                       x.device === 'motor' ? '⚡ Motor/Servo' :
                       x.device === 'advanced' ? '🔌 Advanced' :
                       x.device === 'idle' ? '💤 Idle' : x.device) +
                      ' · ' + (x.role === 'bridge' ? 'BRIDGE' : 'HOST');
    document.getElementById('setupActive').textContent = fmt(a);
    document.getElementById('setupPreferred').textContent = fmt(p);
    document.getElementById('setupPendingBadge').style.display =
      j.reboot_pending ? 'inline-block' : 'none';
    document.getElementById('setupDetails').textContent =
      'USB=' + a.usb + ' · Port B=' + a.port + (a.owner ? ' (owner: ' + a.owner + ')' : '');
    // Pre-select cards from preferred mode
    _setupDevice = p.device; _setupRole = p.role;
    document.querySelectorAll('#setupDevice .action-card').forEach(b => {
      b.classList.toggle('active', b.dataset.device === p.device);
    });
    document.querySelectorAll('#setupRole .action-card').forEach(b => {
      b.classList.toggle('active', b.dataset.role === p.role);
    });
    setupUpdateApplyBtn();
    // Connection rail update
    const rail = document.getElementById('setupRailMode');
    if (rail) rail.textContent = a.port + (a.port === 'IDLE' ? '' : ' @ GP10/11');
    const railD = document.getElementById('setupRailDetails');
    if (railD) railD.textContent = 'USB: ' + a.usb + (a.owner ? ' · owner: ' + a.owner : '');
  }).catch(e => { document.getElementById('setupApplyMsg').textContent = 'Ошибка: ' + e; });
}

// ===== USB mode =====
function usbRefresh() {
  fetch('/api/usb/mode').then(r=>r.json()).then(j=>{
    document.getElementById('usbActive').textContent = j.active_name + ' (#' + j.active + ')';
    document.getElementById('usbPreferred').textContent = j.preferred_name + ' (#' + j.preferred + ')';
    const badge = document.getElementById('usbPendingBadge');
    badge.style.display = j.reboot_pending ? 'inline-block' : 'none';
    const c = document.getElementById('usbModes');
    c.innerHTML = '';
    j.modes.forEach(m => {
      const lab = document.createElement('label');
      lab.style.cssText = 'display:flex;align-items:center;gap:8px;cursor:pointer';
      const r = document.createElement('input');
      r.type = 'radio'; r.name = 'usbmode'; r.value = m.id;
      if (m.id === j.preferred) r.checked = true;
      lab.appendChild(r);
      lab.appendChild(document.createTextNode(m.name));
      c.appendChild(lab);
    });
  }).catch(e => { document.getElementById('usbMsg').textContent = 'Ошибка: ' + e; });
}
function _usbSelectedMode() {
  const r = document.querySelector('input[name=usbmode]:checked');
  return r ? r.value : null;
}
async function usbApplyReboot() {
  const mode = _usbSelectedMode();
  if (mode === null) { document.getElementById('usbMsg').textContent = 'Выберите режим'; return; }
  if (!(await confirmModal('Сохранить USB-режим в NVS и перезагрузить плату?', {okLabel:'Reboot'}))) return;
  const fd = new FormData(); fd.append('mode', mode);
  document.getElementById('usbMsg').textContent = 'Сохраняю...';
  fetch('/api/usb/mode', {method:'POST', body:fd}).then(r=>r.text()).then(t=>{
    document.getElementById('usbMsg').textContent = t + ' → reboot...';
    return fetch('/api/usb/reboot', {method:'POST'});
  }).then(() => {
    document.getElementById('usbMsg').textContent = 'Плата перезагружается, через 5-7с refresh страницы.';
  }).catch(e => { document.getElementById('usbMsg').textContent = 'Ошибка: ' + e; });
}
// ---------- Port B Mode Selector ----------
function portBRefresh() {
  fetch('/api/port/status').then(r=>r.json()).then(j=>{
    document.getElementById('portBPins').textContent =
      '(pin_a=GPIO ' + j.pin_a + ', pin_b=GPIO ' + j.pin_b + ')';
    const m = document.getElementById('portBMode');
    m.textContent = j.mode_name;
    m.className = 'status ' + (j.mode === 0 ? 'off' : 'on');
    document.getElementById('portBOwner').textContent =
      j.owner ? ('owner: ' + j.owner) : '';
    document.getElementById('portBPref').textContent = j.preferred_name;
    const c = document.getElementById('portBModeBtns');
    c.innerHTML = '';
    j.modes.forEach(m => {
      const b = document.createElement('button');
      b.textContent = m.name;
      b.style.flex = '1 1 auto';
      b.style.minWidth = '70px';
      if (m.id === j.preferred) {
        b.className = 'success';
      }
      b.onclick = () => portBSet(m.id);
      c.appendChild(b);
    });
  }).catch(e => {
    document.getElementById('portBMsg').textContent = 'Refresh failed';
  });
}
function portBSet(modeId) {
  const fd = new FormData(); fd.append('mode', modeId);
  fetch('/api/port/preferred', {method:'POST', body:fd})
    .then(r=>r.text()).then(t=>{
      document.getElementById('portBMsg').textContent = t;
      setTimeout(portBRefresh, 100);
    });
}
function portBRelease() {
  fetch('/api/port/release', {method:'POST'})
    .then(r=>r.text()).then(t=>{
      document.getElementById('portBMsg').textContent = t;
      setTimeout(portBRefresh, 100);
    });
}

let cpLogTimer = null;
function cpLogRefresh() {
  fetch('/api/cp2112/log').then(r=>r.text()).then(t=>{
    const el = document.getElementById('cpLog');
    el.textContent = t;
    // Count lines for stats
    const lines = t.split('\n').filter(l => l.trim().length > 0);
    const stats = document.getElementById('cpLogStats');
    if (stats) stats.textContent = lines.length > 1 ? (lines.length - 1) + ' entries' : '';
  });
}
function cpLogExport() {
  const text = document.getElementById('cpLog').textContent;
  const blob = new Blob([text], {type:'text/plain'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'cp2112_log_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.txt';
  a.click();
}
function cpLogAutoToggle() {
  if (cpLogTimer) { clearInterval(cpLogTimer); cpLogTimer = null; }
  if (document.getElementById('cpLogAuto').checked) {
    cpLogTimer = setInterval(cpLogRefresh, 1000);
  }
}

// ===== OTA =====
function fmtBytes(b) {
  if (b >= 1048576) return (b/1048576).toFixed(2) + ' MB';
  if (b >= 1024) return (b/1024).toFixed(1) + ' KB';
  return b + ' B';
}
function otaRefresh() {
  fetch('/api/ota/info').then(r=>r.json()).then(j=>{
    document.getElementById('otaFwVer').textContent   = j.fw_version || '?';
    document.getElementById('otaRunning').textContent = j.running || '?';
    document.getElementById('otaNext').textContent    = j.next    || '?';
    document.getElementById('otaAppSize').textContent = fmtBytes(j.app_size || 0);
    document.getElementById('otaFree').textContent    = fmtBytes(j.app_free || 0);
    document.getElementById('otaSdk').textContent     = j.sdk     || '?';
    document.getElementById('otaRepo').textContent    = j.github_repo || '(не задан)';
    if (j.latest) {
      document.getElementById('otaLatest').textContent = j.latest;
      const outdated = j.latest !== j.fw_version;
      const cmp = document.getElementById('otaCompare');
      cmp.textContent = outdated ? 'доступна новая версия' : 'актуальная';
      cmp.className = 'kv-value ' + (outdated ? 'text-warn' : 'text-success');
      document.getElementById('otaPullBtn').disabled = !outdated || !j.latest_url;
    }
    if (j.pull_running) {
      document.getElementById('otaPullStage').textContent = j.pull_message + ' (' + j.pull_progress + '%)';
      document.getElementById('otaPullBar').style.width = j.pull_progress + '%';
    } else if (j.pull_message) {
      document.getElementById('otaPullStage').textContent = j.pull_message;
    }
    // Connection rail heap info
    const rail = document.getElementById('otaRailHeap');
    if (rail) rail.textContent = 'Free: ' + fmtBytes(j.app_free || 0) + ' · Slot: ' + (j.next || '?');
  }).catch(e=>{
    document.getElementById('otaResult').textContent = 'Info error: ' + e;
  });
}

function otaCheck() {
  const btn = document.getElementById('otaCheckBtn');
  btn.disabled = true;
  const cmp = document.getElementById('otaCompare');
  cmp.textContent = 'проверяю...';
  cmp.className = 'kv-value text-muted';
  fetch('/api/ota/check', {method:'POST'})
    .then(r => r.text().then(body => ({ok: r.ok, status: r.status, body})))
    .then(res => {
      btn.disabled = false;
      if (!res.ok) {
        cmp.textContent = 'ошибка HTTP ' + res.status + ': ' + res.body;
        cmp.className = 'kv-value text-danger';
        return;
      }
      otaRefresh();
    })
    .catch(e => {
      btn.disabled = false;
      cmp.textContent = 'Network error: ' + e;
      cmp.className = 'kv-value text-danger';
    });
}

let otaPullPoll = null;
async function otaPull() {
  if (!(await confirmModal('Скачать и прошить последнюю версию из GitHub?\nПлата перезагрузится.', {okLabel:'Pull & Flash'}))) return;
  const stage = document.getElementById('otaPullStage');
  const bar = document.getElementById('otaPullBar');
  stage.textContent = 'starting';
  bar.style.width = '0%';
  bar.classList.remove('success', 'danger');
  document.getElementById('otaPullBtn').disabled = true;

  fetch('/api/ota/pull', {method:'POST'}).then(r => r.text()).then(t => {
    stage.textContent = t;
    if (otaPullPoll) clearInterval(otaPullPoll);
    otaPullPoll = setInterval(() => {
      fetch('/api/ota/info').then(r => r.json()).then(j => {
        bar.style.width = (j.pull_progress || 0) + '%';
        stage.textContent = (j.pull_message || '') + ' (' + (j.pull_progress || 0) + '%)';
        if (!j.pull_running) {
          clearInterval(otaPullPoll);
          otaPullPoll = null;
          bar.classList.remove('success', 'danger');
          if ((j.pull_message || '').startsWith('OK')) {
            bar.classList.add('success');
            setTimeout(() => location.reload(), 10000);
          } else {
            bar.classList.add('danger');
            document.getElementById('otaPullBtn').disabled = false;
          }
        }
      }).catch(() => { /* during reboot fetch fails — that's OK */ });
    }, 1000);
  });
}
function otaAbort() {
  fetch('/api/ota/abort', {method:'POST'}).then(r=>r.text()).then(t=>{
    document.getElementById('otaResult').textContent = t;
    document.getElementById('otaStage').textContent = 'aborted';
  });
}
async function otaUpload() {
  const f = document.getElementById('otaFile').files[0];
  if (!f) return alert('Выбери firmware.bin');
  if (!(await confirmModal('Прошить ' + f.name + ' (' + fmtBytes(f.size) + ')?\nПлата перезагрузится.', {okLabel:'Upload & Flash'}))) return;

  const fd = new FormData();
  fd.append('update', f, f.name);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/upload');
  const bar = document.getElementById('otaBar');
  const stage = document.getElementById('otaStage');
  const res = document.getElementById('otaResult');
  const btn = document.getElementById('otaBtn');

  btn.disabled = true;
  res.textContent = '';
  res.className = 'field-help';
  stage.textContent = 'uploading...';
  bar.style.width = '0%';
  bar.classList.remove('success', 'danger');

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round(e.loaded * 100 / e.total);
      bar.style.width = pct + '%';
      stage.textContent = 'uploading ' + pct + '% (' + fmtBytes(e.loaded) + '/' + fmtBytes(e.total) + ')';
    }
  };
  xhr.onload = () => {
    btn.disabled = false;
    if (xhr.status === 200) {
      bar.style.width = '100%';
      bar.classList.add('success');
      stage.textContent = 'done — rebooting';
      res.className = 'field-help text-success';
      res.textContent = xhr.responseText + ' — страница обновится автоматически через 8 с';
      setTimeout(() => location.reload(), 8000);
    } else {
      bar.classList.add('danger');
      stage.textContent = 'error';
      res.className = 'field-help text-danger';
      res.textContent = 'HTTP ' + xhr.status + ': ' + xhr.responseText;
    }
  };
  xhr.onerror = () => {
    btn.disabled = false;
    stage.textContent = 'network error';
    res.style.color = '#f44';
    res.textContent = 'Network error';
  };
  xhr.send(fd);
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
// === BATTERY SUB-TABS ===
let _curBattSub = localStorage.getItem('battSub') || 'dji';
let _userPickedBattSub = !!localStorage.getItem('battSubManual');

function showBattSub(sub, userInitiated = true) {
  _curBattSub = sub;
  if (userInitiated) {
    localStorage.setItem('battSub', sub);
    localStorage.setItem('battSubManual', '1');
    _userPickedBattSub = true;
  }
  document.querySelectorAll('#battSubtabs .subtab').forEach(e => {
    e.classList.toggle('active', e.dataset.sub === sub);
  });
  document.querySelectorAll('.batt-dji-only').forEach(e => e.style.display = (sub === 'dji') ? '' : 'none');
  document.querySelectorAll('.batt-clone-only').forEach(e => e.style.display = (sub === 'clone') ? '' : 'none');
  document.querySelectorAll('.batt-generic-only').forEach(e => e.style.display = (sub === 'generic') ? '' : 'none');
  // Start/stop vendor register polling when Clone tab shown
  if (sub === 'clone' && document.getElementById('vrvAuto')?.checked) vrvRefresh();
}

function battSubReset() {
  localStorage.removeItem('battSubManual');
  _userPickedBattSub = false;
  document.getElementById('battSubHint').textContent = 'auto-detect based on connected battery';
}

// Called from telemetry handler — auto-switch sub-tab if user hasn't manually picked
function battSubAutoDetect(devType, chipType, mfr) {
  if (_userPickedBattSub) return;
  let auto = 'generic';
  // devType: 0=none, 1=DJI (by manufacturer name), 2=generic SBS
  if (devType === 1) {
    // DJI-labelled; chipType==0 means MAC not supported → clone
    auto = (chipType === 0) ? 'clone' : 'dji';
  } else if (devType === 2) {
    // Some clones have "PTL" or other non-DJI manufacturer — still treat as clone if chipType==0
    auto = (chipType === 0) ? 'clone' : 'generic';
  }
  // Update hint
  const hint = document.getElementById('battSubHint');
  if (hint) hint.innerHTML = 'auto: <b style="color:var(--accent)">' + auto + '</b> <a href="#" onclick="battSubReset(); return false;" style="color:var(--text-dim);font-size:10px">[reset]</a>';
  if (auto !== _curBattSub) showBattSub(auto, false);
}

// === CHALLENGE HARVESTER (Clone tab) ===
let _chSamples = [];

async function chHarvest() {
  const reg = document.getElementById('chReg').value;
  const val = document.getElementById('chVal').value;
  const count = document.getElementById('chCount').value;
  const readLen = document.getElementById('chRLen').value;
  const inc = document.getElementById('chInc').checked ? '1' : '0';
  const status = document.getElementById('chStatus');
  const res = document.getElementById('chResult');
  const ana = document.getElementById('chAnalysis');
  status.textContent = 'harvesting...';
  res.textContent = '';
  ana.textContent = '';
  const t0 = Date.now();
  try {
    const r = await fetch(`/api/batt/clone/harvest?reg=${reg}&writeVal=${val}&count=${count}&readLen=${readLen}&inc=${inc}`);
    const data = await r.json();
    _chSamples = data;
    document.getElementById('chExportBtn').disabled = false;
    const dt = Date.now() - t0;
    status.textContent = data.length + ' samples in ' + dt + 'ms (' + Math.round(data.length*1000/dt) + ' tx/s)';

    // Compact display
    const lines = [];
    lines.push('idx  write   response (hex)                          varies');
    lines.push('---- ------- ----------------------------------------- ------');
    for (let i = 0; i < Math.min(data.length, 100); i++) {
      const d = data[i];
      const prev = i > 0 ? data[i-1].r : '';
      const diff = diffBytes(prev, d.r);
      lines.push(String(i).padEnd(4) + ' ' + d.w + ' ' + d.r.padEnd(42) + ' ' + diff);
    }
    if (data.length > 100) lines.push('... (' + (data.length - 100) + ' more)');
    res.textContent = lines.join('\n');

    // Quick analysis
    chAnalyze(data);
  } catch (err) {
    status.textContent = 'error: ' + err;
  }
}

function diffBytes(a, b) {
  if (!a || a.length !== b.length) return '?';
  let marks = '';
  for (let i = 0; i < a.length; i += 2) {
    marks += (a.substr(i,2) === b.substr(i,2)) ? '.' : 'X';
  }
  return marks;
}

function chAnalyze(samples) {
  if (samples.length < 4) return;
  // Find byte positions that VARY across all samples
  const sampleLen = samples[0].r.length / 2;
  const varies = new Array(sampleLen).fill(0);  // count of distinct values per byte
  for (let b = 0; b < sampleLen; b++) {
    const set = new Set();
    for (const s of samples) set.add(s.r.substr(b*2, 2));
    varies[b] = set.size;
  }

  // Byte entropy — for bytes that vary
  const ana = document.getElementById('chAnalysis');
  let s = 'Byte variability (distinct values / ' + samples.length + ' samples):\n';
  for (let b = 0; b < sampleLen; b++) {
    const pct = Math.round(varies[b]*100/samples.length);
    const marker = varies[b] === 1 ? 'const' : varies[b] >= samples.length * 0.5 ? 'HIGH-entropy' : 'varies';
    s += `  byte[${b}] = ${varies[b]} distinct (${pct}%) ${marker}\n`;
  }

  // Look for period/repetition in first varying byte
  const firstVar = varies.findIndex(v => v > 1);
  if (firstVar >= 0 && samples.length >= 10) {
    const bytes = samples.map(x => parseInt(x.r.substr(firstVar*2, 2), 16));
    // Check period up to 128
    let period = 0;
    for (let p = 1; p < Math.min(128, bytes.length/2); p++) {
      let match = true;
      for (let i = 0; i < bytes.length - p; i++) {
        if (bytes[i] !== bytes[i+p]) { match = false; break; }
      }
      if (match) { period = p; break; }
    }
    s += '\nByte[' + firstVar + '] period check: ' + (period ? 'PERIOD=' + period + ' detected' : 'no repetition found');
    // First 32 values as hex for quick visual pattern scan
    s += '\nByte[' + firstVar + '] first 32 samples: ' + bytes.slice(0, 32).map(b => b.toString(16).padStart(2,'0')).join(' ');
  }
  ana.textContent = s;
}

function chExport() {
  if (!_chSamples.length) return;
  let csv = 'index,write,read_hex\n';
  _chSamples.forEach((s, i) => {
    csv += i + ',' + s.w + ',' + s.r + '\n';
  });
  const blob = new Blob([csv], {type:'text/csv'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'harvest_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.csv';
  a.click();
}

async function chWBlock() {
  const reg = document.getElementById('wbReg').value;
  const data = document.getElementById('wbData').value.replace(/\s/g,'');
  const raw = document.getElementById('wbRaw').checked;
  if (!/^[0-9a-fA-F]+$/.test(data) || data.length % 2 !== 0) {
    document.getElementById('wbResult').textContent = 'data must be even-length hex';
    return;
  }
  const fd = new FormData();
  fd.append('reg', reg); fd.append('data', data);
  if (raw) fd.append('raw', '1');
  const r = await fetch('/api/batt/clone/wblock', {method:'POST', body:fd}).then(r=>r.json());
  const rb = await fetch('/api/batt/scan/sbs?from=' + reg + '&to=' + reg).then(r=>r.json());
  const readback = rb[0] ? ('word=' + rb[0].word + ' block=' + (rb[0].bhex || '')) : '(no response)';
  document.getElementById('wbResult').textContent =
    (r.ok ? '[OK]' : '[FAIL]') + ' wrote ' + r.len + 'B to ' + reg + (raw ? ' (raw)' : ' (block)') + ' → readback: ' + readback;
}

// === VENDOR REGISTER VIEWER (Clone tab) ===
let _vrvTimer = null;
let _vrvPrev = {};

async function vrvRefresh() {
  const el = document.getElementById('vrvGrid');
  const status = document.getElementById('vrvStatus');
  status.textContent = 'reading...';
  try {
    const r = await fetch('/api/batt/scan/sbs?from=0xD0&to=0xFF');
    const entries = await r.json();
    status.textContent = entries.length + '/48 regs';
    let html = '<table style="width:100%;border-collapse:collapse">';
    html += '<tr style="color:var(--text-dim)"><th style="text-align:left">reg</th><th style="text-align:left">word</th><th style="text-align:left">block (hex)</th><th style="text-align:left">ascii</th></tr>';
    for (const e of entries) {
      const addr = e.reg;
      const prev = _vrvPrev[addr];
      const changed = prev && (prev.word !== e.word || prev.bhex !== e.bhex);
      const rowStyle = changed ? 'background:rgba(255,160,0,0.15);animation:flash 1s' : '';
      html += '<tr style="border-bottom:1px solid var(--border-soft);' + rowStyle + '">';
      html += '<td style="padding:2px 6px;color:var(--accent)">' + addr + '</td>';
      html += '<td style="padding:2px 6px">' + (e.word || '-') + (e.dec !== undefined ? ' (' + e.dec + ')' : '') + '</td>';
      html += '<td style="padding:2px 6px;font-family:monospace">' + (e.bhex || '').slice(0, 32) + '</td>';
      html += '<td style="padding:2px 6px;color:var(--text-dim)">' + (e.bascii || '').replace(/</g,'&lt;').slice(0,16) + '</td>';
      html += '</tr>';
      _vrvPrev[addr] = e;
    }
    html += '</table>';
    el.innerHTML = html;
  } catch (err) {
    status.textContent = 'error: ' + err;
  }
}

function vrvAutoToggle() {
  if (_vrvTimer) { clearInterval(_vrvTimer); _vrvTimer = null; }
  if (document.getElementById('vrvAuto').checked) {
    vrvRefresh();
    _vrvTimer = setInterval(vrvRefresh, 2000);
  }
}

// === LIVE CHART ===
const _chartMax = 180;  // ~3 min at 1Hz telemetry
const _chartVS = [], _chartIS = [];
function chartClear() { _chartVS.length = 0; _chartIS.length = 0; chartRender(); }
function chartPush(mV, mA) {
  _chartVS.push(mV); _chartIS.push(mA);
  if (_chartVS.length > _chartMax) { _chartVS.shift(); _chartIS.shift(); }
  chartRender();
}
function chartRender() {
  if (!_chartVS.length) {
    document.getElementById('chartV').setAttribute('points', '');
    document.getElementById('chartI').setAttribute('points', '');
    return;
  }
  const W = 400, H = 160, pad = 5;
  // Voltage range: auto, with some padding
  const vMin = Math.min(..._chartVS), vMax = Math.max(..._chartVS);
  const vRange = (vMax - vMin) || 1000;
  // Current range: symmetric around zero for readability
  const iMax = Math.max(..._chartIS.map(Math.abs), 1000);
  const n = _chartVS.length;
  const step = (W - 2*pad) / Math.max(_chartMax - 1, 1);
  const vPts = _chartVS.map((v,i) => {
    const x = pad + i * step;
    const y = H - pad - ((v - vMin) / vRange) * (H - 2*pad - 20) - 10;
    return x.toFixed(1) + ',' + y.toFixed(1);
  }).join(' ');
  const iPts = _chartIS.map((v,i) => {
    const x = pad + i * step;
    const y = H/2 - (v / iMax) * (H/2 - pad - 10);
    return x.toFixed(1) + ',' + y.toFixed(1);
  }).join(' ');
  document.getElementById('chartV').setAttribute('points', vPts);
  document.getElementById('chartI').setAttribute('points', iPts);
  const last = n - 1;
  document.getElementById('chartLabelV').textContent = 'V: ' + (_chartVS[last]/1000).toFixed(2) + ' (' + (vMin/1000).toFixed(2) + '-' + (vMax/1000).toFixed(2) + ')';
  document.getElementById('chartLabelI').textContent = 'I: ' + _chartIS[last] + 'mA (±' + iMax + ')';
}

function handleMsg(m) {
  if (m.type === 'batt') {
    const c = m.connected;
    const bc = document.getElementById('battConn');
    bc.className = 'status ' + (c ? 'on' : 'off');
    bc.textContent = c ? 'YES' : 'NO';
    if (!c) return;

    // Device type badge
    const dtEl = document.getElementById('battDevType');
    if (m.devType === 1) { dtEl.style.display='inline-block'; dtEl.textContent='DJI'; dtEl.className='status on'; }
    else if (m.devType === 2) { dtEl.style.display='inline-block'; dtEl.textContent='SBS'; dtEl.className='status on'; }
    else dtEl.style.display='none';

    document.getElementById('battSOC').textContent = m.soc + ' %';
    document.getElementById('battBar').style.width = m.soc + '%';
    document.getElementById('battVolt').textContent = (m.voltage/1000).toFixed(2) + ' V';
    document.getElementById('battCurr').textContent = m.current + ' mA';

    // Push sample to live chart ring buffer
    chartPush(m.voltage, m.current);

    // Auto-switch Battery Lab sub-tab based on detected battery type
    battSubAutoDetect(m.devType, m.chipType, m.mfr);
    document.getElementById('battAvgCurr').textContent = m.avgCurrent + ' mA';
    document.getElementById('battTemp').textContent = m.temp.toFixed(1) + ' °C';
    document.getElementById('battSOH').textContent = (m.soh && m.soh < 0xFFFF) ? m.soh + ' %' : '-';
    document.getElementById('battCycle').textContent = m.cycles;
    document.getElementById('battCap').textContent = m.remain + '/' + m.full + ' mAh';
    document.getElementById('battDesign').textContent = m.design + ' mAh';

    // Time estimates
    const fmtMin = v => (v && v < 0xFFFF) ? v + ' min' : '-';
    document.getElementById('battTTE').textContent = fmtMin(m.ate);
    document.getElementById('battTTF').textContent = fmtMin(m.ttf);
    document.getElementById('battChg').textContent = (m.chgI || '-') + ' mA / ' + (m.chgV ? (m.chgV/1000).toFixed(2)+'V' : '-');
    document.getElementById('battStatusBits').textContent = m.statusDecoded || '-';

    document.getElementById('battMfr').textContent = m.mfr;
    document.getElementById('battDev').textContent = m.dev;
    document.getElementById('battChem').textContent = m.chem || '-';
    document.getElementById('battConfig').textContent = m.cellCount + 'S / ' + m.design + 'mAh / ' + (m.designV ? (m.designV/1000).toFixed(1)+'V' : '-');

    // DJI serial
    const djiSnEl = document.getElementById('battDjiSN');
    if (m.djiSN) { djiSnEl.textContent = m.djiSN; djiSnEl.style.display = ''; djiSnEl.parentElement.style.display = ''; }
    else { djiSnEl.parentElement.style.display = 'none'; }

    // Decode manufacture date (SBS format: bits 15:9=year+1980, 8:5=month, 4:0=day)
    if (m.mfgDate) {
      const y = ((m.mfgDate >> 9) & 0x7F) + 1980;
      const mo = (m.mfgDate >> 5) & 0x0F;
      const d = m.mfgDate & 0x1F;
      document.getElementById('battSN').textContent = '#' + m.sn + ' (' + y + '-' + String(mo).padStart(2,'0') + '-' + String(d).padStart(2,'0') + ')';
    } else {
      document.getElementById('battSN').textContent = '#' + m.sn;
    }
    document.getElementById('battModel').textContent = m.model || '-';

    // Chip info — human-readable
    const chipName = m.chipType === 0x4307 ? 'BQ40Z307' :
                     m.chipType === 0x0550 ? 'BQ30Z55' :
                     m.chipType === 0 ? 'Clone (MAC not supported)' : 'ID: 0x' + m.chipType.toString(16).toUpperCase();
    document.getElementById('battChip').textContent = chipName;
    document.getElementById('battFwHw').textContent = m.fwVer ? ('FW ' + m.fwVer + ' / HW ' + m.hwVer) : 'Not available';

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

    // Key warning — suppress for clones (they use standard TI keys)
    const isClone = (m.mfr === 'PTL' || m.chipType === 0);
    document.getElementById('keyWarning').style.display = (m.needsKey && !isClone) ? 'block' : 'none';

    // Extended status — show decoded first, hex as secondary
    const hex8 = v => '0x' + v.toString(16).toUpperCase().padStart(8, '0');
    document.getElementById('opStatus').textContent = m.opDecoded || 'N/A';
    document.getElementById('opDecoded').textContent = hex8(m.operationStatus);
    document.getElementById('safetyStatus').textContent = m.safetyDecoded || 'OK';
    document.getElementById('safetyDecoded').textContent = hex8(m.safetyStatus);
    const pfEl = document.getElementById('pfDecoded');
    document.getElementById('pfStatus').textContent = m.pfDecoded || 'OK';
    pfEl.textContent = hex8(m.pfStatus);
    pfEl.style.color = m.hasPF ? '#f44' : '#6f6';
    document.getElementById('mfgStatus').textContent = m.mfgDecoded || 'N/A';
    document.getElementById('mfgDecoded').textContent = hex8(m.manufacturingStatus);

    document.getElementById('cellsSync').textContent = (m.cellsSync ? 'sync' : 'async') + ', ' + (m.cellCount || '?') + 'S';
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
    // Drive cross-tab Port B contention banner: if CRSF is running and user
    // is on ELRS tab, warn. Standalone probes/controls in ELRS tab don't
    // auto-pause CRSF (only the Flash button does, server-side).
    const elrsBn = document.getElementById('elrsCrsfBanner');
    if (elrsBn) elrsBn.style.display = (m.enabled ? '' : 'none');

    // Status badges
    const st = document.getElementById('crsfStatus');
    st.textContent = m.enabled ? 'ON' : 'OFF';
    st.className = 'status ' + (m.enabled ? 'on' : 'off');
    const co = document.getElementById('crsfConnected');
    co.textContent = m.connected ? 'LINK' : 'NO LINK';
    co.className = 'status ' + (m.connected ? 'on' : 'off');

    // "No TX link" hint — show after running >3 s with no link validated.
    // Avoids a flash-of-warning immediately after Start.
    const nlHint = document.getElementById('crsfNoLinkHint');
    if (nlHint) {
      const shouldShow = m.enabled && !m.connected && !(m.link && m.link.lq !== undefined);
      if (shouldShow && !window._crsfNoLinkSince) window._crsfNoLinkSince = Date.now();
      if (!shouldShow) window._crsfNoLinkSince = 0;
      const stable = window._crsfNoLinkSince && (Date.now() - window._crsfNoLinkSince > 3000);
      nlHint.style.display = stable ? '' : 'none';
    }

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

    // Device info + parameter tree were removed in the receiver-tab merge.
    // Their data is fetched on-demand via /api/elrs/device_info and
    // /api/elrs/params from the RX Status + rcv-config sections.
  }
  // `flash` WS messages were consumed by the old tab-crsf flash-in-progress
  // banner (removed in the Option A merge). The primary Flash card polls
  // /api/flash/status directly, so no WS handler is needed here.
}

// Theme — restore from localStorage (dark is default)
function applyTheme(t) {
  if (t === 'light') {
    document.documentElement.setAttribute('data-theme', 'light');
    document.getElementById('themeToggle').textContent = '☀';
  } else {
    document.documentElement.removeAttribute('data-theme');
    document.getElementById('themeToggle').textContent = '🌙';
  }
}
function toggleTheme() {
  const cur = localStorage.getItem('theme') || 'dark';
  const next = cur === 'dark' ? 'light' : 'dark';
  localStorage.setItem('theme', next);
  applyTheme(next);
}
applyTheme(localStorage.getItem('theme') || 'dark');

connect();

// Restore last-used workspace + tab from localStorage (or default to Battery Lab)
showWorkspace(_curWs);

// Fetch and display firmware version in footer
fetch('/api/ota/info').then(r=>r.json()).then(j=>{
  const el = document.getElementById('fwFooter');
  if (el) el.textContent = 'FPV MultiTool ' + (j.fw_version || '?') + ' | heap ' + (j.app_size ? (j.app_size/1024/1024).toFixed(1)+'MB' : '?');
}).catch(()=>{});
