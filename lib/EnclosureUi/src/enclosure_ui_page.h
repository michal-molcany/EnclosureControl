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
.tabs{display:flex;gap:4px;margin-bottom:12px;grid-column:1/-1;align-items:center}
.wrap{max-width:900px;margin:0 auto;padding:16px;display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));gap:12px}
.tab{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:8px 16px;cursor:pointer;color:var(--dim);font:inherit}
.tab.active{background:var(--acc);border-color:var(--acc);color:#06121c;font-weight:600}
.tab-content{display:none}
/* The pane must span the whole page grid. It is a grid item of .wrap like the
   header and the tab bar, and without this it sits in column 1 only - which is
   what made the page look left-aligned on any screen wide enough for two
   columns (measured: a 428 px pane inside an 868 px content area). */
.tab-content.active{display:block;grid-column:1/-1}
.mats{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px 14px}
header{grid-column:1/-1;display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:4px}
h1{font-size:18px;margin:0}
.meta{color:var(--dim);font-size:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin-bottom:0}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:0 0 10px}
.subhead{display:flex;align-items:center;gap:8px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:16px 0 6px;padding-top:12px;border-top:1px solid var(--line)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:10px 14px}
.srcs{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px}
.src{border:1px solid var(--line);border-radius:8px;background:#141824;padding:12px 14px}
.src h3{margin:0 0 10px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);font-weight:600}
.src .srcrow{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:9px 0;border-top:1px solid var(--line)}
.src .srcrow:first-of-type{border-top:0}
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
.note.warn{color:var(--bad);font-size:12px;margin-top:10px}
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
.chart{margin-top:12px}
.chart canvas{width:100%;display:block;background:#0e1116;border:1px solid var(--line);border-radius:6px}
.chart canvas+canvas{margin-top:6px}
.lg{display:flex;gap:10px;flex-wrap:wrap;font-size:11px;color:var(--dim);margin-bottom:6px}
.lg i{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:4px;vertical-align:-1px}
.lg i.bed{background:#e6eaf0}
.lg i.ch{background:#4caf7d}
.lg i.tgt{background:#8b97a8}
.lg i.out{background:#5aa9e6}
.lg i.p1{background:rgba(90,140,255,.45)}
.lg i.p2{background:rgba(150,150,170,.45)}
.lg i.p3{background:rgba(224,163,62,.5)}
</style>
</head>
<body>
<div class="wrap">

<nav class="tabs">
  <button class="tab active" data-tab="status">Status</button>
  <button class="tab" data-tab="settings">Settings</button>
  <button class="tab" data-tab="manual">Manual control</button>
  <span class="meta" id="meta" style="margin-left:auto">connecting&hellip;</span>
</nav>

<div class="tab-content active" id="tab-status">
<div class="card">
  <h2>Status</h2>
  <div class="grid">
    <div><div class="k">Chamber</div><div class="v" id="chamber">&ndash;</div></div>
    <div><div class="k">Setpoint</div><div class="v" id="setpoint">&ndash;</div></div>
    <div><div class="k">State</div><div class="v" id="state">&ndash;</div></div>
    <div><div class="k">Mode</div><div class="v" id="mode">&ndash;</div></div>
    <div><div class="k">Material</div><div class="v" id="material">&ndash;</div></div>
    <div><div class="k">Louvre</div><div class="v" id="louvre">&ndash;</div><div class="bar"><i id="louvrebar"></i></div></div>
    <div><div class="k">Fan</div><div class="v" id="fan">&ndash;</div><div class="bar"><i id="fanbar"></i></div></div>
    <div><div class="k">Elapsed</div><div class="v" id="elapsed">&ndash;</div></div>
    <div><div class="k">Remaining</div><div class="v" id="remaining">&ndash;</div></div>
    <div><div class="k">Emergency</div><div class="v" id="emg">&ndash;</div></div>
  </div>
  <div class="note" id="offline" style="display:none">Board unreachable &mdash; retrying every 2&nbsp;s.</div>
</div>

<div class="card">
  <h2>Sources</h2>
  <div class="srcs">
    <div class="src">
      <h3>OctoPrint</h3>
      <div class="srcrow"><span class="k">Status</span><span class="badge" id="srcOcto">online</span></div>
      <div class="srcrow"><span class="k">Job</span><span class="v" id="srcOctoJob">idle</span></div>
      <div class="srcrow"><span class="k">Last poll</span><span class="v" id="srcOctoAge">&ndash;</span></div>
    </div>
    <div class="src">
      <h3>CYD</h3>
      <div class="srcrow"><span class="k">Link</span><span class="badge" id="srcCyd">live</span></div>
      <div class="srcrow"><span class="k">Mode</span><span class="v" id="srcCydMode">auto</span></div>
      <div class="srcrow"><span class="k">RSSI</span><span class="v" id="srcCydRssi">&ndash;</span></div>
    </div>
  </div>
  <div class="note warn" id="octodown" style="display:none">OctoPrint unreachable &mdash; temperatures and job state show the last successful readings.</div>
</div>

<div class="card wide">
  <h2>History</h2>
  <div class="lg">
    <span><i style="background:#e6eaf0"></i>chamber &deg;C</span>
    <span><i style="background:#ff6b6b"></i>setpoint &deg;C</span>
    <span><i style="background:#4caf7d"></i>louvre %</span>
    <span><i style="background:#5aa9e6"></i>fan %</span>
    <span style="margin-left:auto">last 15&nbsp;min</span>
  </div>
  <canvas id="histplot" style="height:240px;width:100%;background:#0e1116;border:1px solid var(--line);border-radius:6px"></canvas>
  <div class="note" id="histmsg">Waiting for samples&hellip;</div>
</div>

</div>
<div class="tab-content" id="tab-settings">
<div class="card">
  <h2>Material profiles</h2>
  <div class="mats">
    <div><div class="k">PLA</div><input id="matpla" type="number" min="20" max="80" step="1"></div>
    <div><div class="k">PETG</div><input id="matpetg" type="number" min="20" max="80" step="1"></div>
    <div><div class="k">FLEX (TPU/TPA)</div><input id="matflex" type="number" min="20" max="80" step="1"></div>
    <div><div class="k">HT (ASA/ABS/Nylon)</div><input id="matht" type="number" min="20" max="80" step="1"></div>
  </div>
  <div class="btns"><button class="primary" id="matsave">Save</button></div>
  <div class="msg" id="matmsg"></div>
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
    oscillates the louvre and fan around a 35&nbsp;&deg;C chamber setpoint, measures the ultimate gain and period,
    then offers new Ziegler&ndash;Nichols gains. The printer must be idle; the 1-hour cap starts when the
    chamber reaches 35&nbsp;&deg;C.</div>
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

</div>
<div class="tab-content" id="tab-manual">
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
</div>

<div class="modal hide" id="calconfirm">
  <div class="panel">
    <h2>Start PID calibration?</h2>
    <ul>
      <li>The printer must be <strong>idle</strong> &mdash; no print or paused job.</li>
      <li>The bed will be heated to <strong>90&nbsp;&deg;C</strong> and switched off when the run ends.</li>
      <li>The louvre and fan run for up to <strong>60&nbsp;minutes after the chamber reaches
        35&nbsp;&deg;C</strong>.</li>
      <li>The chamber is held near 35&nbsp;&deg;C; the run aborts if it exceeds 50&nbsp;&deg;C.</li>
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
      <div class="k">Bed re-asserts</div><div class="v" id="calpbedkeep">&ndash;</div>
    </div>
    <div class="note">The run ends when the oscillation is stable and measurable: at least 6 phases, up to 12 if it has not settled. The total is an estimate and can move.</div>
    <div id="calchartslot"><div class="chart" id="calchart">
      <div class="lg">
        <span><i class="bed"></i>bed</span>
        <span><i class="ch"></i>chamber</span>
        <span><i class="tgt"></i>target</span>
        <span><i class="out"></i>cooling</span>
        <span><i class="p1"></i>preheat</span>
        <span><i class="p2"></i>settle</span>
        <span><i class="p3"></i>relay</span>
      </div>
      <canvas id="calplotbed" style="height:96px"></canvas>
      <canvas id="calplotch" style="height:124px"></canvas>
      <div class="msg" id="calplotmsg">Waiting for the first samples&hellip;</div>
    </div></div>
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
    <div id="calreschart"></div>
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
// Material targets: per-field "edited but not saved yet". The status poll runs
// every 2 s and used to write all four fields unconditionally, so a value typed
// here was replaced by the stored one before Save could be pressed - the save
// then persisted the OLD number. A field is dirty from its first edit until a
// save succeeds, and only dirty fields are skipped by render(); untouched ones
// keep tracking the board, because the plugin can change them too.
var MAT_FIELDS = ['matpla', 'matpetg', 'matflex', 'matht'];
var matDirty = {};
var matBusy = false;
MAT_FIELDS.forEach(function (id) {
  var el = $(id);
  if (!el) { return; }
  el.oninput = function () { matDirty[id] = true; say($('matmsg'), 'unsaved', ''); };
});
// Set by a successful save; consumed by the next render() to blank the field.
var keyJustSaved = false;
// Latest status document, so a local UI action (opening the confirm dialog) can
// repaint the modal without waiting for the next poll.
var lastStatus = null;
// History buffers for the status graph. One entry per successful poll, and the
// page polls every 2 s, so 480 entries span a little over 15 minutes. The span
// is a floor, not a promise: a hidden tab is throttled by the browser and fills
// the same buffer much more slowly, which only makes the window longer.
var HIST_MAX = 480;
var histCh = [];
var histLouvre = [];
var histFan = [];
var histSetpoint = [];
var histMs = [];
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

// Humanised "how long since the last successful poll", for the OctoPrint status
// row. The raw value (ms) is what /api/status reports; the page only ever needs
// the "down for X" part.
function fmtAge(ms) {
  var s = Math.round((ms || 0) / 1000);
  if (s < 60) { return s + ' s'; }
  var m = Math.round(s / 60);
  if (m < 60) { return m + ' m'; }
  var h = Math.round(m / 60);
  return h + ' h ' + (m % 60) + ' m';
}

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

// --- Calibration temperature graph -----------------------------------------
//
// The numbers come from the BOARD (GET api/cal/trace), not from this page's own
// sampling: a run lasts up to an hour, it is usually started from the OctoPrint
// plugin or the CYD so the page is opened late, and a backgrounded tab
// throttles its timers to about one tick a minute.
//
// Two stacked plots sharing a time axis, drawn by hand on canvas. No charting
// library: this page has no external assets by design, so it works on a LAN
// with no internet.

var calTrace = null;      // last api/cal/trace document
var calTraceAt = 0;       // Date.now() of the last fetch
var calWasActive = false; // edge detection for "a run just started / ended"

// Index = phase code: 1 preheat, 2 settle, 3 relay. 0/4/5 get no band.
var CAL_PHASE_TINT = ['', 'rgba(90,140,255,.13)', 'rgba(150,150,170,.13)', 'rgba(224,163,62,.16)'];
var CAL_INVALID = -32768; // ENC_CHAMBER_INVALID

// Smallest "round" number at or above `raw`. Keeps the chamber axis on steps
// a person can read: 2, 2.5, 5, 10 - never span/4 turning a 10 C window into
// 28.4 / 30.9 / 33.4 / 35.9.
function niceStep(raw) {
  var steps = [0.5, 1, 2, 2.5, 5, 10, 20, 50];
  for (var i = 0; i < steps.length; i++) {
    if (raw <= steps[i]) { return steps[i]; }
  }
  return steps[steps.length - 1];
}

// Seconds as a person reads them: "42 s" under a minute, then "6m 35s" and
// "1h 02m 08s". Same format the OctoPrint plugin tab uses, so the two never
// disagree about one run.
function formatElapsed(ms) {
  var total = Math.floor((parseInt(ms, 10) || 0) / 1000);
  if (total < 60) { return total + ' s'; }
  var h = Math.floor(total / 3600);
  var m = Math.floor((total % 3600) / 60);
  var s = total % 60;
  var pad = function (v) { return ('0' + v).slice(-2); };
  if (h) { return h + 'h ' + pad(m) + 'm ' + pad(s) + 's'; }
  return m + 'm ' + pad(s) + 's';
}

// ONE chart element, moved between the progress and the result panel. Moving it
// beats drawing into two canvases: one draw path, and the bitmap survives the
// move (an element cannot be in two places, so appendChild() is a move).
function chartInto(slotId) {
  var chart = $('calchart');
  var slot = $(slotId);
  if (chart && slot && chart.parentNode !== slot) { slot.appendChild(chart); }
}

function maybeFetchTrace(c) {
  if (!(c.active || c.hasResult || c.aborted)) { calWasActive = false; return; }
  var nowMs = Date.now();
  var starting = !!c.active && !calWasActive;
  var settled = !c.active && calWasActive;
  calWasActive = !!c.active;
  // A new run means a new trace. Drop the old one immediately so the graph
  // cannot show the previous experiment for the first seconds of this one.
  if (starting) { calTrace = null; calTraceAt = 0; }
  // While a run is on, refresh every 10 s; the 2 s status poll keeps the live
  // right edge moving in between. The falling edge gets one extra fetch, to
  // pick up the samples the run wrote on its way out.
  var due = starting || settled || (c.active && nowMs - calTraceAt > 10000) ||
            (!c.active && !calTrace && nowMs - calTraceAt > 10000);
  if (!due) { return; }
  calTraceAt = nowMs;
  get('api/cal/trace').then(function (d) {
    calTrace = d;
    drawTrace(lastStatus);
  }).catch(function (err) {
    var msg = $('calplotmsg');
    if (msg) { msg.textContent = 'Temperature history unavailable: ' + err.message; msg.style.display = ''; }
  });
}

function clearPlot(cv) {
  var ctx = cv.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
}

// Sizes the backing store to the CSS box and returns a context in CSS pixels.
// Null while the panel is hidden: clientWidth is 0 there, and resizing the
// canvas to it would discard the bitmap for nothing.
function plotSize(cv) {
  var w = cv.clientWidth, h = cv.clientHeight;
  if (!w || !h) { return null; }
  var dpr = window.devicePixelRatio || 1;
  var bw = Math.round(w * dpr), bh = Math.round(h * dpr);
  if (cv.width !== bw) { cv.width = bw; }
  if (cv.height !== bh) { cv.height = bh; }
  var ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx: ctx, w: w, h: h };
}

function drawTrace(s) {
  var bedCv = $('calplotbed'), chCv = $('calplotch'), msg = $('calplotmsg');
  if (!bedCv || !chCv) { return; }
  var d = calTrace;
  if (!d || !d.n) {
    clearPlot(bedCv); clearPlot(chCv);
    if (msg) { msg.textContent = 'Waiting for the first samples\u2026'; msg.style.display = ''; }
    return;
  }
  if (msg) { msg.style.display = 'none'; }

  var t = d.t, chRaw = d.ch, bedRaw = d.bed, outRaw = d.out || [], ph = d.ph || [], n = d.n;
  var cal = (s && s.cal) || {};
  var padL = 34, padR = 6, padT = 8, padB = 14;

  // The freshest reading from the status poll, so the right edge keeps moving
  // between the 10 s trace refreshes. Same units as the trace (0.1 C).
  var live = null;
  if (cal.active && s && s.chamberValid) {
    live = { t: Math.floor((cal.elapsed || 0) / 1000), ch: s.chamber, bed: s.bed };
  }
  var lastT = Math.max(t[n - 1], live ? live.t : 0);
  var xMax = Math.max(300, Math.ceil(lastT / 60) * 60); // 5 min floor, whole minutes
  var xStep = xMax <= 900 ? 120 : (xMax <= 1800 ? 300 : 600);

  function map(cv, yMin, yMax) {
    var p = plotSize(cv);
    if (!p) { return null; }
    // Clear on EVERY frame. plotSize() only resets the canvas when its backing
    // size changes, which is once; without this the previous frame stays
    // underneath. The translucent phase bands then stack until they are solid,
    // and every step of the time window leaves its labels, gridlines and curve
    // behind as ghosts.
    p.ctx.clearRect(0, 0, p.w, p.h);
    p.X = function (sec) { return padL + (sec / xMax) * (p.w - padL - padR); };
    p.Y = function (v) { return p.h - padB - ((v - yMin) / (yMax - yMin)) * (p.h - padT - padB); };
    p.yMin = yMin;
    p.yMax = yMax;
    return p;
  }

  // `step` is the tick step in degrees, so the labels are round numbers.
  function axes(p, step) {
    if (!p) { return; }
    var ctx = p.ctx, k, v, y, x, sec;
    var ticks = Math.round((p.yMax - p.yMin) / step);
    var whole = (step % 1 === 0);
    ctx.font = '10px system-ui,sans-serif';
    ctx.lineWidth = 1;
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'right';
    for (k = 0; k <= ticks; k++) {
      v = p.yMin + k * step;
      y = Math.round(p.Y(v)) + 0.5;
      ctx.strokeStyle = '#2a313d';
      ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(p.w - padR, y); ctx.stroke();
      ctx.fillStyle = '#8b97a8';
      ctx.fillText(whole ? String(Math.round(v)) : v.toFixed(1), padL - 4, y);
    }
    for (sec = xStep; sec <= xMax; sec += xStep) {
      x = Math.round(p.X(sec)) + 0.5;
      // A tick at the very end of the window would put its label half outside
      // the canvas, so the edge tick is skipped rather than clipped.
      if (x > p.w - 12) { continue; }
      ctx.strokeStyle = '#2a313d';
      ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, p.h - padB); ctx.stroke();
    }
  }

  // Minute labels belong under the bottom plot only.
  function xAxis(p) {
    if (!p) { return; }
    var ctx = p.ctx, sec, x;
    ctx.font = '10px system-ui,sans-serif';
    ctx.fillStyle = '#8b97a8';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (sec = xStep; sec <= xMax; sec += xStep) {
      x = p.X(sec);
      if (x > p.w - 12) { continue; } // same edge rule as the gridlines
      ctx.fillText((sec / 60) + 'm', x, p.h - padB + 2);
    }
  }

  function bands(p) {
    if (!p) { return; }
    var ctx = p.ctx, i = 0, code, start, end;
    // One rect per contiguous run: per-sample rects would leave seams and cost
    // a few hundred calls per redraw for no visible difference.
    while (i < n) {
      code = ph[i];
      if (!(code >= 1 && code <= 3)) { i++; continue; }
      start = i;
      while (i < n && ph[i] === code) { i++; }
      end = (i < n) ? t[i] : lastT;
      ctx.fillStyle = CAL_PHASE_TINT[code];
      ctx.fillRect(p.X(t[start]), padT, Math.max(1, p.X(end) - p.X(t[start])),
                   p.h - padT - padB);
    }
  }

  // Polyline with a gap at every invalid sample, plus the live point when the
  // status poll has a fresher reading than the trace does.
  function line(p, values, color, width, dash, liveRaw) {
    if (!p) { return; }
    var ctx = p.ctx, i, x, y, pen = false;
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    if (dash) { ctx.setLineDash(dash); }
    ctx.beginPath();
    for (i = 0; i < n; i++) {
      if (values[i] === CAL_INVALID) { pen = false; continue; }
      x = p.X(t[i]);
      y = p.Y(values[i] / 10);
      if (pen) { ctx.lineTo(x, y); } else { ctx.moveTo(x, y); pen = true; }
    }
    if (liveRaw !== undefined && liveRaw !== CAL_INVALID && pen) {
      ctx.lineTo(p.X(live.t), p.Y(liveRaw / 10));
    }
    ctx.stroke();
    ctx.restore();
  }

  function hline(p, value, color, dash) {
    if (!p) { return; }
    var ctx = p.ctx;
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1;
    ctx.setLineDash(dash);
    ctx.beginPath();
    ctx.moveTo(padL, p.Y(value));
    ctx.lineTo(p.w - padR, p.Y(value));
    ctx.stroke();
    ctx.restore();
  }

  // Chamber range: fit the data, but never tighter than 10 C, or a 1 C
  // oscillation is a flat line. The setpoint stays in view even before the
  // climb gets there. The window is then snapped to the tick step, and grown
  // a gap at a time until it covers the curve, so no sample is ever drawn on
  // the axis edge.
  var setpoint = cal.setpoint ? cal.setpoint : 35;
  var lo = setpoint - 1, hi = setpoint + 1, i, v;
  for (i = 0; i < n; i++) {
    if (chRaw[i] === CAL_INVALID) { continue; }
    v = chRaw[i] / 10;
    if (v < lo) { lo = v; }
    if (v > hi) { hi = v; }
  }
  if (live && live.ch !== CAL_INVALID) {
    v = live.ch / 10;
    if (v < lo) { lo = v; }
    if (v > hi) { hi = v; }
  }
  var need = Math.max(10, hi - lo + 2);
  var chStep = niceStep(need / 5);
  var gaps = 5;
  var mid = (lo + hi) / 2;
  var chLo, chHi;
  for (;;) {
    chLo = Math.floor((mid - (gaps * chStep) / 2) / chStep) * chStep;
    chHi = chLo + gaps * chStep;
    if ((chHi >= hi + 0.5 && chLo <= lo - 0.5) || gaps >= 8) { break; }
    gaps += 1;
  }

  var bedP = map(bedCv, 20, 100);
  var chP = map(chCv, chLo, chHi);

  bands(bedP); bands(chP);
  axes(bedP, 20); axes(chP, chStep);
  xAxis(chP);

  // Chamber: the relay's noise band, then the setpoint through its middle.
  if (chP) {
    var c2 = chP.ctx;
    c2.fillStyle = 'rgba(139,151,168,.07)';
    c2.fillRect(padL, chP.Y(setpoint + 0.5), chP.w - padL - padR,
                Math.max(1, chP.Y(setpoint - 0.5) - chP.Y(setpoint + 0.5)));
  }
  hline(chP, setpoint, '#8b97a8', [4, 3]);
  hline(bedP, cal.bedTarget ? cal.bedTarget : 90, '#8b97a8', [4, 3]);

  // Relay "cooling on" as a 3 px strip along the bottom of the chamber plot:
  // it shows when the relay switched without needing another axis.
  if (chP && outRaw.length) {
    var c3 = chP.ctx, a, b;
    c3.fillStyle = 'rgba(90,169,230,.6)';
    for (i = 0; i < n; i++) {
      if (outRaw[i] >= 100) {
        a = chP.X(t[i]);
        b = chP.X((i + 1 < n) ? t[i + 1] : lastT);
        c3.fillRect(a, chP.h - padB - 4, Math.max(1, b - a), 3);
      }
    }
  }

  line(bedP, bedRaw, '#e6eaf0', 1.6, null, live ? live.bed : undefined);
  line(chP, chRaw, '#4caf7d', 1.6, null, live ? live.ch : undefined);
}

// The calibration modal is driven entirely by the board's status document,
// except for the first screen: the confirmation exists only in the browser,
// because the board must not be asked to do anything until the user has
// acknowledged the 90 C bed and the 60-minute run.
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
    $('calptime').textContent = formatElapsed(c.elapsed);
    $('calpcycles').textContent = (c.cycles || 0) + ' / ~' + (c.cyclesExpected || c.cycles || 6);
    var _bk = (c.bedRefreshes || 0);
    if (c.bedRefreshNextMs) { _bk += ' (next in ' + Math.ceil(c.bedRefreshNextMs / 60000) + 'm)'; }
    $('calpbedkeep').textContent = _bk;
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
    $('calptime').textContent = formatElapsed(c.elapsed);
    $('calpcycles').textContent = (c.cycles || 0) + ' / ~' + (c.cyclesExpected || c.cycles || 6);
    var _bk = (c.bedRefreshes || 0);
    if (c.bedRefreshNextMs) { _bk += ' (next in ' + Math.ceil(c.bedRefreshNextMs / 60000) + 'm)'; }
    $('calpbedkeep').textContent = _bk;
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

  // The graph follows whichever dialog is on screen. It is the same element,
  // moved into that panel, so there is one canvas and one draw path.
  chartInto((c.hasResult && !c.active) ? 'calreschart' : 'calchartslot');
  maybeFetchTrace(c);
  if (c.active || c.hasResult || c.aborted) { drawTrace(s); }
}

function drawHist() {
  var cv = $('histplot');
  if (!cv) { return; }
  var p = plotSize(cv);
  if (!p) { return; }
  var ctx = p.ctx, w = p.w, h = p.h;
  ctx.clearRect(0, 0, w, h);
  var n = histCh.length;
  if (n < 2) {
    $('histmsg').style.display = '';
    $('histmsg').textContent = 'Waiting for samples\u2026';
    return;
  }
  $('histmsg').style.display = 'none';
  var padL = 34, padR = 40, padT = 8, padB = 18;
  var chMin = Infinity, chMax = -Infinity;
  for (var i = 0; i < n; i++) {
    if (histCh[i] < chMin) { chMin = histCh[i]; }
    if (histCh[i] > chMax) { chMax = histCh[i]; }
    if (histSetpoint[i] !== null) {
      if (histSetpoint[i] < chMin) { chMin = histSetpoint[i]; }
      if (histSetpoint[i] > chMax) { chMax = histSetpoint[i]; }
    }
  }
  var chRange = chMax - chMin;
  if (chRange < 1) { chRange = 5; }
  var pad = chRange * 0.15;
  chMin = Math.floor((chMin - pad) / 5) * 5;
  chMax = Math.ceil((chMax + pad) / 5) * 5;
  if (chMin < 0) { chMin = 0; }
  if (chMax - chMin < 5) { chMax = chMin + 5; }
  var yMaxR = chMax, yMinR = chMin;
  var stepR = (yMaxR - yMinR) <= 20 ? 5 : 10;
  var yMaxL = 100, yMinL = 0;
  var stepL = 20;
  ctx.strokeStyle = '#2a313d';
  ctx.lineWidth = 1;
  ctx.font = '9px system-ui,sans-serif';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = '#8b97a8';
  for (var v = yMinL; v <= yMaxL; v += stepL) {
    var y = h - padB - ((v - yMinL) / (yMaxL - yMinL)) * (h - padT - padB);
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(w - padR, y);
    ctx.stroke();
    ctx.fillText(v, padL - 4, y);
  }
  ctx.strokeStyle = '#8b97a8';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(w - padR, padT);
  ctx.lineTo(w - padR, h - padB);
  ctx.stroke();
  ctx.textAlign = 'left';
  ctx.fillStyle = '#e6eaf0';
  ctx.font = '10px system-ui,sans-serif';
  for (var v = yMinR; v <= yMaxR; v += stepR) {
    var y = h - padB - ((v - yMinR) / (yMaxR - yMinR)) * (h - padT - padB);
    ctx.fillText(v + '\u00b0C', w - padR + 4, y);
  }
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  ctx.fillStyle = '#8b97a8';
  ctx.font = '9px system-ui,sans-serif';
  // Time span covered by the buffer. Minimum 1 s so two samples taken inside
  // the same second still produce a sane divisor.
  var t0 = histMs[0];
  var t1 = histMs[n - 1];
  if (t1 - t0 < 1000) { t1 = t0 + 1000; }
  var now = Date.now();
  var numLabels = Math.min(5, n);
  // Ticks are chosen on the time axis and then snapped to the nearest sample,
  // so a stretch of throttled polls cannot bunch four labels into one corner.
  for (var li = 0; li < numLabels; li++) {
    var target = t0 + (li * (t1 - t0)) / (numLabels - 1);
    var idx = 0;
    while (idx < n - 1 && histMs[idx] < target) { idx++; }
    var x = xOf(idx);
    var ago = Math.round((now - histMs[idx]) / 1000);
    var label = ago === 0 ? 'now' : (ago < 60 ? '-' + ago + 's' : '-' + Math.floor(ago / 60) + 'm');
    ctx.fillText(label, x, h - padB + 4);
  }
  // The x axis is time, not sample index: a paused or throttled poll then shows
  // up as a real gap instead of being stretched across the whole plot.
  function xOf(i) { return padL + ((histMs[i] - t0) / (t1 - t0)) * (w - padL - padR); }
  function yOfL(v) { return h - padB - ((v - yMinL) / (yMaxL - yMinL)) * (h - padT - padB); }
  function yOfR(v) { return h - padB - ((v - yMinR) / (yMaxR - yMinR)) * (h - padT - padB); }
  ctx.strokeStyle = '#e6eaf0';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (var i = 0; i < n; i++) {
    var x = xOf(i);
    var y = yOfR(histCh[i]);
    if (i === 0) { ctx.moveTo(x, y); } else { ctx.lineTo(x, y); }
  }
  ctx.stroke();
  ctx.strokeStyle = '#ff6b6b';
  ctx.lineWidth = 1.5;
  ctx.setLineDash([4, 3]);
  ctx.beginPath();
  var spStarted = false;
  for (var i = 0; i < n; i++) {
    if (histSetpoint[i] === null) { spStarted = false; continue; }
    var x = xOf(i);
    var y = yOfR(histSetpoint[i]);
    if (!spStarted) { ctx.moveTo(x, y); spStarted = true; }
    else { ctx.lineTo(x, y); }
  }
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.strokeStyle = '#4caf7d';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (var i = 0; i < n; i++) {
    var x = xOf(i);
    var y = yOfL(histLouvre[i]);
    if (i === 0) { ctx.moveTo(x, y); } else { ctx.lineTo(x, y); }
  }
  ctx.stroke();
  ctx.strokeStyle = '#5aa9e6';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (var i = 0; i < n; i++) {
    var x = xOf(i);
    var y = yOfL(histFan[i]);
    if (i === 0) { ctx.moveTo(x, y); } else { ctx.lineTo(x, y); }
  }
  ctx.stroke();
}

function render(s) {
  lastStatus = s;
  $('offline').style.display = 'none';
  $('octodown').style.display = s.octoStale ? '' : 'none';
  // OctoPrint source: the status pill lives in the Sources card. The numbers the
  // page would otherwise call "the bed/chamber temperature" are keep-last-good,
  // so on a stale read they are history, not present.

  // Sources panel: OctoPrint status pill and supporting details.
  var octoBadge = $('srcOcto');
  if (octoBadge) {
    octoBadge.className = 'badge ' + (s.octoStale ? 'bad' : 'ok');
    octoBadge.textContent = s.octoStale ? 'DOWN' : 'online';
  }
  $('srcOctoJob').textContent = s.job ? 'printing' : 'idle';
  $('srcOctoAge').textContent = s.octoStale ? ('down ' + fmtAge(s.octoAgeMs)) : 'healthy';

  // Sources panel: CYD link status pill and supporting details.
  var cydBadge = $('srcCyd');
  if (cydBadge) {
    cydBadge.className = 'badge ' + (!s.peer ? 'bad' : (s.remote ? 'ok' : 'warn'));
    cydBadge.textContent = !s.peer ? 'no peer' : (s.remote ? 'live' : 'silent');
  }
  $('srcCydMode').textContent = s.mode === 1 ? 'manual' : 'auto';
  $('srcCydRssi').textContent = (s.rssi === undefined || s.rssi === null) ? '\u2013' : (s.rssi + ' dBm');

  // ENC_CHAMBER_INVALID is INT16_MIN. Showing it as a number would be a lie by
  // two orders of magnitude, so an invalid reading is spelled out instead.
  $('chamber').textContent = s.chamberValid ? (s.chamber / 10).toFixed(1) + ' \u00b0C' : 'no data';
  $('state').textContent = s.emergency ? 'EMERGENCY' : (STATES[s.state] || '?');
  $('mode').textContent = s.mode === 1 ? 'manual' : 'auto';
  $('setpoint').textContent = s.setpoint < 0 ? '\u2013' : s.setpoint + ' \u00b0C';
  $('material').textContent = MATERIALS[s.material] || '?';
  $('elapsed').textContent = s.elapsed || '\u2013';
  $('remaining').textContent = s.remaining || '\u2013';
  $('louvre').textContent = s.louvre + '%';
  $('fan').textContent = s.fan + '%' + (s.fanActive ? ' (on)' : '');
  $('louvrebar').style.width = pct(s.louvre) + '%';
  $('fanbar').style.width = pct(s.fan) + '%';
  $('emg').textContent = s.emergency ? 'ACTIVE' : 'no';

  // Only fill fields the user is not editing - see matDirty above. A dirty
  // field holds the typed value until the save is accepted (or fails, so it can
  // be corrected).
  var mats = { matpla: s.matPla, matpetg: s.matPetg, matflex: s.matFlex, matht: s.matHt };
  MAT_FIELDS.forEach(function (id) {
    if (mats[id] === undefined || matDirty[id]) { return; }
    $(id).value = mats[id];
  });



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

  // --- Status history graph ---
  if (s.chamberValid) {
    histCh.push(s.chamber / 10.0);
    histLouvre.push(s.louvre);
    histFan.push(s.fan);
    histSetpoint.push(s.setpoint >= 0 ? s.setpoint : null);
    histMs.push(Date.now());
    if (histCh.length > HIST_MAX) {
      histCh.shift(); histLouvre.shift(); histFan.shift(); histSetpoint.shift(); histMs.shift();
    }
  }
  drawHist();
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

// Material save button
function saveMaterials() {
  if (matBusy) { return; }
  var body = {};
  for (var i = 0; i < MAT_FIELDS.length; i++) {
    body[MAT_FIELDS[i].slice(3)] = parseInt($(MAT_FIELDS[i]).value, 10);
  }
  matBusy = true;
  say($('matmsg'), 'saving...', '');
  post('api/materials', body).then(function () {
    say($('matmsg'), 'Saved.', 'ok');
    // Accepted, so the fields may go back to following the board. The write
    // answers {ok:true} rather than a status document, so the values come from
    // the poll below - which is also what surfaces a clamped or rejected number.
    matDirty = {};
  }).catch(function (err) {
    // Rejected values (out of range, or a field left blank) keep their dirty
    // flag and stay on screen so they can be corrected instead of silently
    // reverting.
    say($('matmsg'), err.message, 'err');
  }).then(function () {
    matBusy = false;
    poll();
  });
}
$('matsave').onclick = saveMaterials;
MAT_FIELDS.forEach(function (id) {
  $(id).onkeydown = function (ev) { if (ev.key === 'Enter') { saveMaterials(); } };
});

// Tab switching
document.querySelectorAll('.tab').forEach(function(btn) {
  btn.onclick = function() {
    document.querySelectorAll('.tab').forEach(function(b) { b.classList.remove('active'); });
    document.querySelectorAll('.tab-content').forEach(function(c) { c.classList.remove('active'); });
    btn.classList.add('active');
    document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
  };
});

poll();
</script>
</body>
</html>
)HTML";
    } // namespace page
} // namespace EnclosureUi

#endif // ENCLOSURE_UI_PAGE_H