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
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:10px 14px}
.k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.06em}
.v{font-size:18px;font-variant-numeric:tabular-nums}
.bar{height:7px;background:#0e1116;border-radius:4px;overflow:hidden;margin-top:5px}
.bar>i{display:block;height:100%;background:var(--acc);width:0}
.row{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.row2{display:grid;grid-template-columns:2fr 1fr;gap:10px}
input[type=number]{width:100%;background:#0e1116;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:7px 8px;font:inherit}
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
</div>

<div class="card">
  <h2>OctoPrint address <span class="badge" id="octosrc">&nbsp;</span></h2>
  <div class="row2">
    <div><div class="k">IP address</div><input id="octoip" type="text" inputmode="decimal" placeholder="192.168.1.88"></div>
    <div><div class="k">Port</div><input id="octoport" type="number" min="1" max="65535" step="1"></div>
  </div>
  <div class="btns">
    <button class="primary" id="octosave">Check and save</button>
    <button id="octoreset">Reset to firmware default</button>
  </div>
  <div class="note" id="octonote"></div>
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

function render(s) {
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

  // Sliders show what the BOARD holds, not what this page last sent. They are
  // left alone while the user has one focused or is dragging, so a poll cannot
  // yank a handle out from under a finger.
  var mm = $('manualmode');
  if (mm !== document.activeElement) { mm.checked = (s.mode === 1); }
  $('sliders').className = (s.mode === 1) ? '' : 'hide';
  if (document.activeElement !== $('louvres')) { $('louvres').value = s.manualLouvre; $('louvrev').textContent = s.manualLouvre; }
  if (document.activeElement !== $('fanres')) { $('fanres').value = s.manualFan; $('fanrev').textContent = s.manualFan; }
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
// GETs /api/version on the address first and refuses to store it if nothing
// answers. The failure text is the board's own, naming what went wrong.
$('octosave').onclick = function () {
  if (octoBusy) { return; }
  octoBusy = true;
  say($('octomsg'), 'checking ' + $('octoip').value + ' ...', '');
  post('api/octoprint', { ip: $('octoip').value, port: parseInt($('octoport').value, 10) || 80 })
    .then(function (s) {
      octoFilled = false;
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
  say($('octomsg'), 'restoring the firmware default ...', '');
  post('api/octoprint/defaults')
    .then(function (s) {
      octoFilled = false;
      say($('octomsg'), 'Back to the address compiled into the firmware.', 'ok');
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

poll();
</script>
</body>
</html>
)HTML";
    } // namespace page
} // namespace EnclosureUi

#endif // ENCLOSURE_UI_PAGE_H