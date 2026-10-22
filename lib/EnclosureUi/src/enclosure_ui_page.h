#ifndef ENCLOSURE_UI_PAGE_H
#define ENCLOSURE_UI_PAGE_H

// The whole web UI as one string constant.
//
// WHY THIS IS A STRING AND NOT A FILESYSTEM
// ------------------------------------------
// Neither board has a LittleFS partition, and the cheapest way to add one (a
// custom partition table, a data upload step, a flash offset to get wrong) costs
// far more than embedding the page. It is also the more reliable option: with no
// filesystem there is no state in which the page is missing and the firmware has
// to decide what to serve instead.
//
// WHY THERE IS NO PROGMEM HERE
// ----------------------------
// On ESP32, PROGMEM expands to nothing and pgm_read_byte() is a plain
// dereference: .rodata is memory-mapped flash, so a const char array is already
// readable in place (see cores/esp32/pgmspace.h). Adding PROGMEM here would
// suggest a constraint this platform does not have. What does matter is that the
// page must not be COPIED into RAM - hence send_P() in enclosure_ui.cpp, which
// streams it straight from flash, rather than send(), which builds a String.
//
// No external assets: no CDN, no fonts, no separate CSS or JS file. Every byte
// comes off the board in one request, so the page works on a LAN with no
// internet access - which is the situation this firmware is normally in.
//
// The JSON field names must match the ones enclosure_ui.cpp emits in
// /api/status. They are the wire format of this page, not an implementation
// detail: change one and change both.

namespace EnclosureUi
{
    // Named `page`, not `detail`: ArduinoJson has its own `detail` namespace, and
    // an unqualified `detail::` reference inside a translation unit that also
    // includes it resolves to the wrong one.
    namespace page
    {
        // Raw string literal so the JS and CSS need no escaping of quotes,
        // backslashes or newlines - the single most common way a hand-written
        // HTML page breaks at compile time.
        static const char kPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Enclosure Fan Control</title>
<style>
:root{--bg:#12151a;--card:#1b2029;--line:#2a313d;--fg:#e6eaf0;--dim:#8b97a8;--ok:#4caf7d;--warn:#e0a33e;--bad:#e05c5c;--acc:#5aa9e6}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:840px;margin:0 auto;padding:16px}
header{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:12px}
h1{font-size:18px;margin:0}
.meta{color:var(--dim);font-size:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin-bottom:12px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:0 0 10px}
.subhead{display:flex;align-items:center;gap:8px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:16px 0 6px;padding-top:12px;border-top:1px solid var(--line)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:10px 14px}
.k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.06em}
.v{font-size:18px;font-variant-numeric:tabular-nums}
.bar{height:7px;background:#0e1116;border-radius:4px;overflow:hidden;margin-top:5px}
.bar>i{display:block;height:100%;background:var(--acc);width:0}
.row{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.row2{display:grid;grid-template-columns:2fr 1fr;gap:10px}
input[type=number]{width:100%;background:#0e1116;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:7px 8px;font:inherit}
input[type=password]{width:100%;background:#0e1116;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:7px 8px;font:inherit}
input[type=text]{width:100%;background:#0e1116;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:7px 8px;font:inherit}
input[type=range]{width:100%;accent-color:var(--acc)}
button{background:#232a35;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:8px 12px;font:inherit;cursor:pointer}
button:hover{border-color:var(--acc)}
button.primary{background:var(--acc);border-color:var(--acc);color:#06121c;font-weight:600}
button:disabled{opacity:.5;cursor:default}
.btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.badge{display:inline-block;padding:1px 8px;border-radius:999px;font-size:11px;border:1px solid var(--line);color:var(--dim);vertical-align:middle}
.badge.ok{color:var(--ok);border-color:var(--ok)}
.badge.warn{color:var(--warn);border-color:var(--warn)}
.badge.bad{color:var(--bad);border-color:var(--bad)}
.note{color:var(--dim);font-size:12px;margin-top:10px}
.chk{display:flex;align-items:center;gap:8px;cursor:pointer}
.msg{font-size:12px;margin-top:8px;min-height:16px}
.msg.err{color:var(--bad)}
.msg.ok{color:var(--ok)}
.hide{display:none}
.modal{position:fixed;inset:0;background:rgba(4,7,11,.72);display:flex;align-items:center;justify-content:center;padding:16px;z-index:20}
.modal.hide{display:none}
.panel{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;width:100%;max-width:560px;max-height:88vh;overflow:auto}
.panel h2{font-size:16px;margin:0 0 10px}
.panel ul{margin:8px 0 0 18px;padding:0}
.panel li{margin:3px 0}
table.gains{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;margin-top:4px}
table.gains th{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.06em;font-weight:400;padding:4px 6px;text-align:right}
table.gains th:first-child{text-align:left}
table.gains td{padding:4px 6px;border-top:1px solid var(--line);text-align:right}
table.gains td:first-child{text-align:left;color:var(--dim)}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:13px;margin-top:10px}
.kv .k{text-transform:none;letter-spacing:0}
.kv .v{font-size:14px}
.warnbox{border:1px solid var(--warn);color:var(--warn);border-radius:6px;padding:8px 10px;margin-top:10px;font-size:12px;line-height:1.4}
</style>
</head>
<body>
<div class="wrap">

<header>
  <h1>Enclosure Fan Control</h1>
  <span class="meta" id="meta">connecting&hellip;</span>
</header>

<div class="card">
  <h2>Status</h2>
  <div class="grid">
    <div><div class="k">Chamber</div><div class="v" id="chamber">&ndash;</div></div>
    <div><div class="k">State</div><div class="v" id="state">&ndash;</div></div>
    <div><div class="k">Mode</div><div class="v" id="mode">&ndash;</div></div>
    <div><div class="k">Setpoint</div><div class="v" id="setpoint">&ndash;</div></div>
    <div><div class="k">Material</div><div class="v" id="material">&ndash;</div></div>
    <div><div class="k">Job</div><div class="v" id="job">&ndash;</div></div>
    <div><div class="k">Louvre</div><div class="v" id="louvre">&ndash;</div><div class="bar"><i id="louvrebar"></i></div></div>
    <div><div class="k">Fan</div><div class="v" id="fan">&ndash;</div><div class="bar"><i id="fanbar"></i></div></div>
    <div><div class="k">CYD link</div><div class="v" id="link">&ndash;</div></div>
    <div><div class="k">Emergency</div><div class="v" id="emg">&ndash;</div></div>
  </div>
  <div class="note" id="offline" style="display:none">Board unreachable &mdash; retrying every 2&nbsp;s.</div>
</div>

<div class="card">
  <h2>PID gains <span class="badge" id="savedbadge">saved state</span></h2>
  <div class="row">
    <div><div class="k">kp</div><input id="kp" type="number" step="0.1" inputmode="decimal"></div>
    <div><div class="k">ki</div><input id="ki" type="number" step="0.01" inputmode="decimal"></div>
    <div><div class="k">kd</div><input id="kd" type="number" step="0.1" inputmode="decimal"></div>
  </div>
  <div class="btns">
    <button class="primary" id="apply">Apply</button>
    <button id="save">Save to NVS</button>
    <button id="defaults">Restore defaults</button>
  </div>
  <div class="note">Apply changes the gains the regulator is running on. That is RAM only &mdash; they are lost on
    reboot until you press <em>Save</em>, which writes them to the board's NVS.</div>
  <div class="msg" id="pidmsg"></div>

  <div class="subhead">PID calibration <span class="badge" id="calbadge">idle</span></div>
  <div class="note">Runs a relay-feedback auto-tune (the method ESPHome uses). It heats the bed to 90&nbsp;&deg;C,
    oscillates the louvre and fan around a 40&nbsp;&deg;C chamber setpoint, measures the ultimate gain and period,
    then offers new Ziegler&ndash;Nichols gains. The printer must be idle; the 20-minute cap starts when the
    chamber reaches 40&nbsp;&deg;C.</div>
  <div class="btns"><button class="primary" id="calstart">Calibrate</button></div>
  <div class="msg" id="calmsg"></div>
</div>

<div class="card">
  <h2>OctoPrint server <span class="badge" id="octosrc">&nbsp;</span></h2>
  <div class="row2">
    <div><div class="k">IP address</div><input id="octoip" type="text" inputmode="decimal" placeholder="192.168.1.88"></div>
    <div><div class="k">Port</div><input id="octoport" type="number" min="1" max="65535" step="1"></div>
  </div>
  <div style="margin-top:10px">
    <div class="k">API key <span class="badge" id="keysrc">&nbsp;</span></div>
    <input id="octokey" type="password" autocomplete="off" spellcheck="false"
           placeholder="leave blank to keep the current key" style="margin-top:5px">
  </div>
  <div class="btns">
    <button class="primary" id="octosave">Check and save</button>
    <button id="octoreset">Reset both to firmware defaults</button>
  </div>
  <div class="note" id="octonote"></div>
  <div class="note" id="keynote"></div>
  <div class="msg" id="octomsg"></div>
</div>

<div class="card">
  <h2>Manual control</h2>
  <label class="chk"><input type="checkbox" id="manualmode"> Take over &mdash; fan and louvre follow the sliders</label>
  <div id="sliders" class="hide" style="margin-top:12px">
    <div class="k">Louvre <span id="louvrev">0</span>%</div>
    <input id="louvres" type="range" min="0" max="100" value="0">
    <div class="k" style="margin-top:10px">Fan <span id="fanrev">0</span>%</div>
    <input id="fanres" type="range" min="0" max="100" value="0">
    <div class="btns"><button class="primary" id="sendmanual">Send to board</button></div>
  </div>
  <div class="note" id="cydwarn">The CYD re-sends its own mode every 2&nbsp;s while it is linked, so a manual change made
    here only holds while the display is unlinked.</div>
  <div class="msg" id="manmsg"></div>
</div>

</div>

<div class="modal hide" id="calconfirm">
  <div class="panel">
    <h2>Start PID calibration?</h2>
    <ul>
      <li>The printer must be <strong>idle</strong> &mdash; no print or paused job.</li>
      <li>The bed will be heated to <strong>90&nbsp;&deg;C</strong> and switched off when the run ends.</li>
      <li>The louvre and fan run for up to <strong>20&nbsp;minutes after the chamber reaches
        40&nbsp;&deg;C</strong> (30&nbsp;minutes in total).</li>
      <li>The chamber is held near 40&nbsp;&deg;C; the run aborts if it exceeds 50&nbsp;&deg;C.</li>
      <li>Commands from the CYD are <strong>ignored</strong> for the duration.</li>
    </ul>
    <label class="chk" style="margin-top:12px"><input type="checkbox" id="calack"> I understand and want to proceed</label>
    <div class="msg err" id="calerr"></div>
    <div class="btns">
      <button class="primary" id="calgo" disabled>Start</button>
      <button id="calclose">Cancel</button>
    </div>
  </div>
</div>

<div class="modal hide" id="calprogress">
  <div class="panel">
    <h2 id="calptitle">Calibrating&hellip;</h2>
    <div class="kv">
      <div class="k">Phase</div><div class="v" id="calpphase">&ndash;</div>
      <div class="k">Chamber</div><div class="v" id="calpchamber">&ndash;</div>
      <div class="k">Bed</div><div class="v" id="calpbed">&ndash;</div>
      <div class="k">Elapsed (total)</div><div class="v" id="calptime">&ndash;</div>
      <div class="k">Relay phases</div><div class="v" id="calpcycles">&ndash;</div>
    </div>
    <div class="warnbox hide" id="calpwarn"></div>
    <div class="btns">
      <button id="calpcancel">Cancel calibration</button>
      <button id="calpdismiss" class="hide">Dismiss</button>
    </div>
  </div>
</div>

<div class="modal hide" id="calresult">
  <div class="panel">
    <h2>New PID gains</h2>
    <table class="gains">
      <tr><th></th><th>old</th><th>new</th></tr>
      <tr><td>kp</td><td id="calroldkp">&ndash;</td><td id="calrnewkp">&ndash;</td></tr>
      <tr><td>ki</td><td id="calroldki">&ndash;</td><td id="calrnewki">&ndash;</td></tr>
      <tr><td>kd</td><td id="calroldkd">&ndash;</td><td id="calrnewkd">&ndash;</td></tr>
    </table>
    <div class="kv">
      <div class="k">Ku</div><div class="v" id="calrku">&ndash;</div>
      <div class="k">Tu</div><div class="v" id="calrtu">&ndash;</div>
      <div class="k">Amplitude</div><div class="v" id="calramp">&ndash;</div>
      <div class="k">Relay half-span (d)</div><div class="v" id="calrdelta">&ndash;</div>
    </div>
    <div class="warnbox hide" id="calrwarn"></div>
    <div class="btns">
      <button class="primary" id="calrapply">Apply</button>
      <button id="calrapplysafe">Apply + save to NVS</button>
      <button id="calrdiscard">Discard</button>
    </div>
    <div class="msg" id="calrmsg"></div>
  </div>
</div>
<script>
var $ = function (id) { return document.getElementById(id); };
var STATES = ['Idle', 'PLA', 'PETG', 'ASA'];
var MATERIALS = ['PLA', 'PETG', 'ASA', 'ABS', 'TPU', 'Nylon', 'High-temp', 'Unknown'];

// Set once the gains have been filled from the board, and again after every
// successful write. While false, a status refresh never touches the inputs -
// otherwise a poll arriving mid-edit would overwrite the value being typed.
var pidFilled = false;
var pidBusy = false;
var octoFilled = false;
var octoBusy = false;
// Set by a successful save; consumed by the next render() to blank the field.
var keyJustSaved = false;
// Latest status document, so a local UI action (opening the confirm dialog) can
// repaint the modal without waiting for the next poll.
var lastStatus = null;
// True while the client-side confirmation dialog is open. Purely a view state:
// the board knows nothing about it until Start is pressed.
var calConfirm = false;
var CAL_PHASES = ['', 'preheating bed', 'settling', 'relay autotune', 'done', 'aborted'];

function say(el, text, cls) {
  el.textContent = text;
  el.className = 'msg' + (cls ? ' ' + cls : '');
}

function get(path) {
  return fetch(path, { cache: 'no-store' }).then(function (r) {
    if (r.status === 401) { throw new Error('Not authorised - reload the page and log in again.'); }
    if (!r.ok) { throw new Error('HTTP ' + r.status); }
    return r.json();
  });
}

// The board answers a rejected write with its own plain-text reason (which range
// was hit, or that the JSON was wrong). That text is the useful part, so it is
// surfaced verbatim rather than replaced with a status code.
function post(path, body) {
  return fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {})
  }).then(function (r) {
    if (r.status === 401) { throw new Error('Not authorised - reload the page and log in again.'); }
    return r.text().then(function (t) {
      if (!r.ok) { throw new Error(t || ('HTTP ' + r.status)); }
      try { return JSON.parse(t); } catch (e) { return {}; }
    });
  });
}

function pct(v) { return Math.max(0, Math.min(100, v | 0)); }

function fmt1(v) {
  if (v === undefined || v === null || isNaN(v)) { return '\u2013'; }
  return (Math.round(v * 10) / 10).toFixed(1);
}
function fmt2(v) {
  if (v === undefined || v === null || isNaN(v)) { return '\u2013'; }
  return (Math.round(v * 100) / 100).toFixed(2);
}
function fmt3(v) {
  if (v === undefined || v === null || isNaN(v)) { return '\u2013'; }
  return (Math.round(v * 1000) / 1000).toFixed(3);
}
function showModal(id) { $(id).classList.remove('hide'); }
function hideModal(id) { $(id).classList.add('hide'); }

// The calibration modal is driven entirely by the board's status document,
// except for the first screen: the confirmation exists only in the browser,
// because the board must not be asked to do anything until the user has
// acknowledged the 90 C bed and the 20-minute run.
function renderCal(s) {
  var c = s.cal || {};
  var badge = $('calbadge');
  if (c.active) {
    badge.textContent = CAL_PHASES[c.phase] || 'running';
    badge.className = 'badge warn';
  } else if (c.hasResult) {
    badge.textContent = 'result ready';
    badge.className = 'badge ok';
  } else if (c.aborted) {
    badge.textContent = 'stopped';
    badge.className = 'badge bad';
  } else {
    badge.textContent = 'idle';
    badge.className = 'badge';
  }
  $('calstart').disabled = !!(c.active || c.hasResult || c.aborted || s.job);

  // The relay owns the outputs for the duration, so the board ignores manual
  // commands. Disable the same controls here rather than letting the page claim
  // a change took effect. PID writes and an OctoPrint address change are also
  // held off: the bed-off at the end is sent through the address in force now.
  var busy = !!c.active;
  ['apply', 'save', 'defaults', 'kp', 'ki', 'kd',
   'octoip', 'octoport', 'octokey', 'octosave', 'octoreset',
   'manualmode', 'louvres', 'fanres', 'sendmanual'].forEach(function (id) {
    var el = $(id);
    if (el) { el.disabled = busy; }
  });

  // A job starting mid-confirmation closes the dialog: the button is disabled
  // from the next poll, and there is no point leaving a Start button that the
  // board would refuse anyway.
  if (calConfirm && !c.active && !c.hasResult && !s.job) { showModal('calconfirm'); }
  else {
    hideModal('calconfirm');
    if (s.job) { calConfirm = false; }
  }

  if (c.active) {
    hideModal('calresult');
    showModal('calprogress');
    $('calptitle').textContent = 'Calibrating\u2026';
    $('calpphase').textContent = CAL_PHASES[c.phase] || '?';
    $('calpchamber').textContent = s.chamberValid ? (s.chamber / 10).toFixed(1) + ' \u00b0C' : 'no data';
    $('calpbed').textContent = s.bedValid ? (s.bed / 10).toFixed(1) + ' \u00b0C (target ' + fmt1(c.bedTarget) + ' \u00b0C)' : 'no data';
    $('calptime').textContent = Math.floor((c.elapsed || 0) / 1000) + ' s';
    $('calpcycles').textContent = c.cycles || 0;
    $('calpcancel').classList.remove('hide');
    $('calpdismiss').classList.add('hide');
    $('calmsg').textContent = 'Calibration running \u2014 manual and CYD commands are ignored.';
  } else if (c.aborted) {
    hideModal('calresult');
    showModal('calprogress');
    $('calptitle').textContent = 'Calibration stopped';
    $('calpphase').textContent = 'aborted';
    $('calpchamber').textContent = s.chamberValid ? (s.chamber / 10).toFixed(1) + ' \u00b0C' : 'no data';
    $('calpbed').textContent = s.bedValid ? (s.bed / 10).toFixed(1) + ' \u00b0C' : 'no data';
    $('calptime').textContent = Math.floor((c.elapsed || 0) / 1000) + ' s';
    $('calpcycles').textContent = c.cycles || 0;
    $('calpwarn').textContent = c.error || 'The calibration did not finish.';
    $('calpwarn').classList.remove('hide');
    $('calpcancel').classList.add('hide');
    $('calpdismiss').classList.remove('hide');
    $('calmsg').textContent = '';
  } else if (c.hasResult) {
    hideModal('calprogress');
    showModal('calresult');
    $('calroldkp').textContent = fmt3(c.oldKp);
    $('calroldki').textContent = fmt3(c.oldKi);
    $('calroldkd').textContent = fmt3(c.oldKd);
    $('calrnewkp').textContent = fmt3(c.kp);
    $('calrnewki').textContent = fmt3(c.ki);
    $('calrnewkd').textContent = fmt3(c.kd);
    $('calrku').textContent = fmt2(c.ku) + ' output/\u00b0C';
    $('calrtu').textContent = fmt1(c.tu) + ' s';
    $('calramp').textContent = fmt2(c.amp) + ' \u00b0C';
    $('calrdelta').textContent = fmt1(c.delta) + ' output units';
    var warnings = [];
    if (c.clamped) { warnings.push('One or more gains were clamped to the accepted range (kp \u2264 50, ki \u2264 5, kd \u2264 50).'); }
    if (!c.zcSym) { warnings.push('The oscillation was not symmetrical; the result may be inaccurate.'); }
    if (!c.ampConv) { warnings.push('The oscillation had not fully settled; the result may be inaccurate.'); }
    if (warnings.length) {
      $('calrwarn').textContent = warnings.join(' ');
      $('calrwarn').classList.remove('hide');
    } else {
      $('calrwarn').classList.add('hide');
    }
    $('calmsg').textContent = '';
  } else {
    hideModal('calprogress');
    hideModal('calresult');
    $('calmsg').textContent = s.job
      ? 'Calibration is disabled while the printer is busy.'
      : '';
  }
}

function render(s) {
  lastStatus = s;
  $('offline').style.display = 'none';

  // ENC_CHAMBER_INVALID is INT16_MIN. Showing it as a number would be a lie by
  // two orders of magnitude, so an invalid reading is spelled out instead.
  $('chamber').textContent = s.chamberValid ? (s.chamber / 10).toFixed(1) + ' \u00b0C' : 'no data';
  $('state').textContent = s.emergency ? 'EMERGENCY' : (STATES[s.state] || '?');
  $('mode').textContent = s.mode === 1 ? 'manual' : 'auto';
  $('setpoint').textContent = s.setpoint < 0 ? '\u2013' : s.setpoint + ' \u00b0C';
  $('material').textContent = MATERIALS[s.material] || '?';
  $('job').textContent = s.job ? 'printing' : 'idle';
  $('louvre').textContent = s.louvre + '%';
  $('fan').textContent = s.fan + '%' + (s.fanActive ? ' (on)' : '');
  $('louvrebar').style.width = pct(s.louvre) + '%';
  $('fanbar').style.width = pct(s.fan) + '%';
  $('link').textContent = !s.peer ? 'no peer' : (s.remote ? 'live' : 'silent');
  $('emg').textContent = s.emergency ? 'ACTIVE' : 'no';

  var badge = $('savedbadge');
  if (s.pidSaved) { badge.textContent = 'saved'; badge.className = 'badge ok'; }
  else { badge.textContent = 'unsaved changes'; badge.className = 'badge warn'; }

  var parts = [];
  if (s.fw) { parts.push('fw ' + s.fw); }
  if (s.ip) { parts.push(s.ip); }
  if (s.rssi !== undefined) { parts.push(s.rssi + ' dBm'); }
  if (s.heap) { parts.push('heap ' + Math.round(s.heap / 1024) + ' KB'); }
  if (s.uptime !== undefined) {
    var u = s.uptime | 0;
    var h = Math.floor(u / 3600), m = Math.floor((u % 3600) / 60);
    parts.push('up ' + (h ? h + 'h ' : '') + m + 'm');
  }
  $('meta').textContent = parts.join(' \u00b7 ');

  if (!pidBusy && !pidFilled) { fillPid(s.pid); pidFilled = true; }

  // Same rule as the PID inputs: never overwrite a field the user is editing.
  // octoFilled goes false after a save so the next poll snaps the fields to
  // whatever the board actually stored, including a correction we made on its
  // behalf.
  if (!octoBusy && !octoFilled) {
    if (document.activeElement !== $('octoip')) { $('octoip').value = s.octoIp || ''; }
    if (document.activeElement !== $('octoport')) { $('octoport').value = s.octoPort || ''; }
    octoFilled = true;
  }
  var src = $('octosrc');
  if (s.octoOverridden) {
    src.textContent = 'saved on this board'; src.className = 'badge ok';
    $('octonote').textContent = 'This board is using the saved address, not the one compiled into the firmware.';
  } else {
    src.textContent = 'firmware default'; src.className = 'badge';
    $('octonote').textContent = 'No saved address on this board: it is using OCTOPRINT_IP from include/octoprint_endpoint.h.';
  }
  $('octonote').textContent += s.octoSentToCyd
    ? ' The CYD is being told the same address.'
    : ' The CYD is not linked, so it has not been told yet.';

  // API key provenance only. The value itself is never in /api/status and is
  // never written into the field, so there is nothing here to fill in - the
  // badge is the whole point. Leaving the field blank on a submit means "keep
  // the key currently in force", which is why showing WHICH key that is matters.
  var ksrc = $('keysrc');
  if (s.octoKeyOverridden) {
    ksrc.textContent = 'saved on this board'; ksrc.className = 'badge ok';
    $('keynote').textContent = 'The key in force was saved through this page. It is never sent back to the browser, which is why the field is blank - paste a new one only if you are changing it.';
  } else if (s.octoKeySet) {
    ksrc.textContent = 'firmware default'; ksrc.className = 'badge';
    $('keynote').textContent = 'The key in force comes from include/secrets.h. It is never sent to the browser, so a blank field keeps that same key.';
  } else {
    ksrc.textContent = 'none set'; ksrc.className = 'badge bad';
    $('keynote').textContent = 'No API key is configured anywhere, so this board cannot read the printer. Paste one above.';
  }
  // Clear the field after a successful save so the credential does not linger in
  // the DOM (and out of any screenshot of the tab).
  if (keyJustSaved) { $('octokey').value = ''; keyJustSaved = false; }

  // Sliders show what the BOARD holds, not what this page last sent. They are
  // left alone while the user has one focused or is dragging, so a poll cannot
  // yank a handle out from under a finger.
  var mm = $('manualmode');
  if (mm !== document.activeElement) { mm.checked = (s.mode === 1); }
  $('sliders').className = (s.mode === 1) ? '' : 'hide';
  if (document.activeElement !== $('louvres')) { $('louvres').value = s.manualLouvre; $('louvrev').textContent = s.manualLouvre; }
  if (document.activeElement !== $('fanres')) { $('fanres').value = s.manualFan; $('fanrev').textContent = s.manualFan; }

  renderCal(s);
}

function fillPid(p) {
  if (!p) { return; }
  $('kp').value = p.kp;
  $('ki').value = p.ki;
  $('kd').value = p.kd;
  ['kp', 'ki', 'kd'].forEach(function (k) {
    var el = $(k);
    if (el) { el.min = p.min[k]; el.max = p.max[k]; }
  });
}

// Errors are the expected case here, not the exception: the board is on a Wi-Fi
// link to a laptop, and either can walk out of range. So a failure shows as a
// line of text and the loop keeps polling rather than throwing into the console.
function poll() {
  get('api/status').then(function (s) {
    render(s);
  }).catch(function (err) {
    $('offline').style.display = '';
  }).then(function () {
    // Chained rather than setInterval: a slow board must not queue up requests
    // behind each other.
    setTimeout(poll, 2000);
  });
}

function gains() {
  return { kp: parseFloat($('kp').value), ki: parseFloat($('ki').value), kd: parseFloat($('kd').value) };
}

function withPid(el, label, path, body) {
  if (pidBusy) { return; }
  pidBusy = true;
  say(el, label + '...', '');
  var work = body ? post(path, body) : post(path, {});
  work.then(function (s) {
    say(el, 'OK.', 'ok');
    pidFilled = false;
    if (s && s.pid) { fillPid(s.pid); pidFilled = true; }
    if (s && s.pidSaved !== undefined) { $('savedbadge').textContent = s.pidSaved ? 'saved' : 'unsaved changes'; $('savedbadge').className = s.pidSaved ? 'badge ok' : 'badge warn'; }
  }).catch(function (err) {
    say(el, err.message, 'err');
  }).then(function () {
    pidBusy = false;
    poll();
  });
}

$('apply').onclick = function () { withPid($('pidmsg'), 'applying', 'api/pid', gains()); };
$('save').onclick = function () { withPid($('pidmsg'), 'saving', 'api/pid/save'); };
$('defaults').onclick = function () { withPid($('pidmsg'), 'restoring', 'api/pid/defaults'); };

// "Check and save" because that is literally what the button does: the board
// GETs /api/version on the address first, using the key pasted in the field if
// there is one and the key already in force if the field is blank, and refuses
// to store anything unless that answers. The failure text is the board's own,
// naming what went wrong.
//
// ONE request for the whole pair, and that is not a simplification. A different
// OctoPrint server means a different address AND a different key, while a probe
// can only use one stored value plus one candidate - so setting either half on
// its own would be tested against the other half and fail, and neither could
// ever be applied first. Sending them together is the only way the pair is
// settable without a serial flash.
$('octosave').onclick = function () {
  if (octoBusy) { return; }
  var key = $('octokey').value;
  octoBusy = true;
  say($('octomsg'), 'checking ' + $('octoip').value + (key.length ? ' with the new key' : '') + ' ...', '');
  post('api/octoprint', {
    ip: $('octoip').value,
    port: parseInt($('octoport').value, 10) || 80,
    key: key
  })
    .then(function (s) {
      octoFilled = false;
      keyJustSaved = key.length > 0;
      say($('octomsg'), 'Answered - saved on this board and being sent to the CYD.', 'ok');
      if (s) { render(s); }
    })
    .catch(function (err) {
      say($('octomsg'), err.message, 'err');
    })
    .then(function () { octoBusy = false; poll(); });
};

$('octoreset').onclick = function () {
  if (octoBusy) { return; }
  octoBusy = true;
  say($('octomsg'), 'restoring the firmware defaults ...', '');
  post('api/octoprint/defaults')
    .then(function (s) {
      octoFilled = false;
      keyJustSaved = true;
      say($('octomsg'), 'Back to the address and key compiled into the firmware.', 'ok');
      if (s) { render(s); }
    })
    .catch(function (err) {
      say($('octomsg'), err.message, 'err');
    })
    .then(function () { octoBusy = false; poll(); });
};

$('manualmode').onchange = function () {
  var on = this.checked;
  $('sliders').className = on ? '' : 'hide';
  sendManual(on);
};

$('louvres').oninput = function () { $('louvrev').textContent = this.value; };
$('fanres').oninput = function () { $('fanrev').textContent = this.value; };
$('sendmanual').onclick = function () { sendManual($('manualmode').checked); };

function sendManual(on) {
  say($('manmsg'), 'sending...', '');
  post('api/manual', {
    mode: on ? 1 : 0,
    louvre: parseInt($('louvres').value, 10),
    fan: parseInt($('fanres').value, 10)
  }).then(function () {
    say($('manmsg'), on ? 'Manual mode on - the board is following the sliders.' : 'Back to automatic regulation.', 'ok');
    poll();
  }).catch(function (err) {
    say($('manmsg'), err.message, 'err');
  });
}

// --- PID calibration -------------------------------------------------------

$('calstart').onclick = function () {
  $('calack').checked = false;
  $('calgo').disabled = true;
  say($('calerr'), '', '');
  // A new run starts with a clean progress/result message, so a failure from
  // the previous run cannot be mistaken for this one's.
  $('calpwarn').classList.add('hide');
  say($('calrmsg'), '', '');
  calConfirm = true;
  renderCal(lastStatus || {});
};

$('calack').onchange = function () { $('calgo').disabled = !this.checked; };
$('calclose').onclick = function () { calConfirm = false; hideModal('calconfirm'); };

$('calgo').onclick = function () {
  if (!$('calack').checked) { return; }
  $('calgo').disabled = true;
  say($('calerr'), 'starting\u2026', '');
  post('api/calibrate', { action: 'start' }).then(function (s) {
    calConfirm = false;
    $('calpwarn').classList.add('hide');
    if (s) { render(s); }
  }).catch(function (err) {
    // The board's refusal is the useful text ("the printer is not idle", ...),
    // so it is shown as-is and the dialog stays open for the user to fix it.
    say($('calerr'), err.message, 'err');
    $('calgo').disabled = false;
  });
};

$('calpcancel').onclick = function () {
  post('api/calibrate', { action: 'cancel' }).then(function (s) {
    if (s) { render(s); }
  }).catch(function (err) {
    // Keep the warnbox class: assigning textContent rather than using say()
    // avoids turning the box into a bare .msg line.
    $('calpwarn').textContent = err.message;
    $('calpwarn').classList.remove('hide');
  });
};

$('calpdismiss').onclick = function () {
  post('api/calibrate', { action: 'discard' }).then(function (s) {
    if (s) { render(s); }
  }).catch(function (err) {
    say($('calmsg'), err.message, 'err');
  });
};

function applyCalibration(save) {
  say($('calrmsg'), save ? 'applying and saving\u2026' : 'applying\u2026', '');
  post('api/calibrate', { action: 'apply' }).then(function (s) {
    // force the PID card to pick up the gains the board now reports
    pidFilled = false;
    if (s) { render(s); }
    if (!save) {
      say($('pidmsg'), 'Calibration gains applied (RAM only \u2014 use Save to NVS to keep them).', 'ok');
      return;
    }
    // Separate chain: apply has consumed the result dialog, so a save failure
    // has to be reported in the PID card or it would never be seen.
    post('api/pid/save').then(function (s2) {
      pidFilled = false;
      if (s2) { render(s2); }
      say($('pidmsg'), 'Calibration gains applied and saved to NVS.', 'ok');
    }).catch(function (err) {
      say($('pidmsg'), 'Gains applied, but saving to NVS failed: ' + err.message, 'err');
    });
  }).catch(function (err) {
    say($('calrmsg'), err.message, 'err');
  });
}

$('calrapply').onclick = function () { applyCalibration(false); };
$('calrapplysafe').onclick = function () { applyCalibration(true); };
$('calrdiscard').onclick = function () {
  post('api/calibrate', { action: 'discard' }).then(function (s) {
    if (s) { render(s); }
  }).catch(function (err) {
    say($('calrmsg'), err.message, 'err');
  });
};

poll();
</script>
</body>
</html>
)HTML";
    } // namespace page
} // namespace EnclosureUi

#endif // ENCLOSURE_UI_PAGE_H