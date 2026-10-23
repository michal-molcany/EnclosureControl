'use strict';

// Runs the SHIPPED page JavaScript (extracted from enclosure_ui_page.h) against
// a stub DOM and a recording 2-D context, and checks the calibration graph.
//
// Why bother: the graph is drawn by hand on a canvas, and a canvas quietly does
// nothing when it is given nonsense - an off-by-one scale, a missing clear, a
// `null` context - and no test of the C side would notice. This executes the
// same text the board serves, so the code under test is the code that ships.
//
// Run: node lib/EnclosureUi/test/selftest_trace.js

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE = path.join(__dirname, '..', 'src', 'enclosure_ui_page.h');

let failures = 0;
let checks = 0;

function check(name, condition, detail) {
  checks += 1;
  if (condition) {
    console.log('  ok    ' + name);
  } else {
    failures += 1;
    console.log('  FAIL  ' + name + (detail === undefined ? '' : ' - ' + detail));
  }
}

// --- extract the script out of the C++ raw string ---------------------------

const header = fs.readFileSync(PAGE, 'utf8');
const html = header.slice(header.indexOf('R"HTML(') + 7, header.indexOf(')HTML"'));
const script = html.slice(html.indexOf('<script>') + 8, html.lastIndexOf('</script>'));
if (!script.includes('function drawTrace')) {
  throw new Error('drawTrace not found - did the extraction break?');
}

// --- stub DOM ---------------------------------------------------------------

function makeContext() {
  const ctx = {
    calls: [],
    transforms: [],
  };
  const record = (name) => function () {
    ctx.calls.push({ name: name, args: Array.prototype.slice.call(arguments) });
  };
  ['setTransform', 'clearRect', 'fillRect', 'beginPath', 'moveTo', 'lineTo',
   'stroke', 'fillText', 'save', 'restore', 'setLineDash', 'closePath', 'fill',
   'arc'].forEach((name) => { ctx[name] = record(name); });
  ctx.count = (name) => ctx.calls.filter((c) => c.name === name).length;
  ctx.of = (name) => ctx.calls.filter((c) => c.name === name);
  ctx.reset = () => { ctx.calls.length = 0; };
  return ctx;
}

function makeElement(id) {
  const classes = new Set();
  const el = {
    id: id,
    children: [],
    parentNode: null,
    textContent: '',
    className: '',
    value: '',
    checked: false,
    disabled: false,
    onclick: null,
    clientWidth: 300,
    clientHeight: 100,
    width: 0,
    height: 0,
    style: {},
    _ctx: null,
    classList: {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
    },
    appendChild: function (child) {
      if (child.parentNode) {
        const siblings = child.parentNode.children;
        const at = siblings.indexOf(child);
        if (at >= 0) { siblings.splice(at, 1); }
      }
      child.parentNode = this;
      this.children.push(child);
      return child;
    },
    getContext: function () {
      if (!this._ctx) { this._ctx = makeContext(); }
      return this._ctx;
    },
  };
  return el;
}

const elements = new Map();
function element(id) {
  if (!elements.has(id)) { elements.set(id, makeElement(id)); }
  return elements.get(id);
}

// Canvases are the only elements with a real drawing surface, and the ones the
// graph talks to have fixed CSS heights.
element('calplotbed').clientHeight = 96;
element('calplotch').clientHeight = 124;

const document = {
  activeElement: null,
  getElementById: (id) => element(id),
};

// --- fake network -----------------------------------------------------------

const net = { status: null, trace: null, calls: [] };

function response(doc) {
  return {
    ok: true,
    status: 200,
    json: () => Promise.resolve(doc),
    text: () => Promise.resolve(JSON.stringify(doc)),
  };
}

function fetchStub(url) {
  net.calls.push(url);
  if (url.indexOf('cal/trace') >= 0) {
    return Promise.resolve(response(net.trace || { interval: 5, n: 0, t: [], ch: [], bed: [], out: [], ph: [] }));
  }
  return new Promise(() => {}); // the status poll is not what is under test
}

const sandbox = {
  document: document,
  window: { devicePixelRatio: 2 },
  fetch: fetchStub,
  console: console,
  Date: Date,
  Math: Math,
  JSON: JSON,
  isNaN: isNaN,
  parseInt: parseInt,
  parseFloat: parseFloat,
  Promise: Promise,
  setTimeout: () => {},
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(script, sandbox, { filename: 'enclosure_ui_page.js' });

const flush = () => new Promise((resolve) => setImmediate(resolve));

// --- helpers ----------------------------------------------------------------

function statusDoc(cal) {
  return {
    chamber: 268,
    chamberValid: true,
    bed: 290,
    bedValid: true,
    // renderCal reads these on the way through
    job: false,
    cal: Object.assign({
      active: false, hasResult: false, aborted: false, phase: 0,
      elapsed: 0, cycles: 0, setpoint: 35, bedTarget: 90, error: '',
    }, cal || {}),
  };
}

// A run: 120 samples at 5 s. Preheat for the first 40, settle for 10, relay for
// the rest; the chamber climbs and then oscillates around 35 C.
function makeTrace(points) {
  const n = points || 120;
  const t = [], ch = [], bed = [], out = [], ph = [];
  for (let i = 0; i < n; i++) {
    t.push(i * 5);
    const inRelay = i >= 50;
    ph.push(i < 40 ? 1 : (i < 50 ? 2 : 3));
    const climb = 26 + Math.min(1, i / 50) * 9; // 26 -> 35 over the first 50
    ch.push(Math.round((inRelay ? 35 + 1.5 * Math.sin((i - 50) / 4) : climb) * 10));
    bed.push(Math.round(Math.min(90, 25 + i * 0.6) * 10));
    out.push(inRelay && (Math.floor((i - 50) / 8) % 2 === 0) ? 200 : 0);
  }
  return { interval: 5, n: n, t: t, ch: ch, bed: bed, out: out, ph: ph };
}

// fillRect is used for both the phase bands (plot height) and the 3 px
// "cooling on" strip, so the two are counted apart.
function rects(ctx) {
  const all = ctx.of('fillRect');
  return {
    all: all.length,
    bands: all.filter((c) => c.args[3] > 10).length,
    strips: all.filter((c) => c.args[3] <= 10).length,
  };
}

function drawCounts(fn) {
  const bedCtx = element('calplotbed').getContext();
  const chCtx = element('calplotch').getContext();
  bedCtx.reset();
  chCtx.reset();
  fn();
  return {
    bed: { lineTo: bedCtx.count('lineTo'), moveTo: bedCtx.count('moveTo'), rects: rects(bedCtx), clear: bedCtx.count('clearRect'), text: bedCtx.count('fillText') },
    ch: { lineTo: chCtx.count('lineTo'), moveTo: chCtx.count('moveTo'), rects: rects(chCtx), clear: chCtx.count('clearRect'), text: chCtx.count('fillText') },
  };
}

// --- tests ------------------------------------------------------------------

(async function run() {
  console.log('calibration trace graph');

  // 1. Empty trace: the message is up, the plots are blank, nothing is drawn
  //    from the previous run.
  sandbox.calTrace = null;
  sandbox.lastStatus = statusDoc({ active: true });
  let counts = drawCounts(() => sandbox.drawTrace(sandbox.lastStatus));
  check('empty trace clears both plots', counts.bed.clear === 1 && counts.ch.clear === 1,
        JSON.stringify(counts));
  check('empty trace draws no line', counts.ch.lineTo === 0, counts.ch.lineTo);
  check('empty trace shows the waiting message',
        /Waiting for the first samples/.test(element('calplotmsg').textContent),
        element('calplotmsg').textContent);

  // 2. A full run: phase bands, bed line, chamber line, axis labels.
  sandbox.calTrace = makeTrace(120);
  counts = drawCounts(() => sandbox.drawTrace(sandbox.lastStatus));

  check('chamber line is drawn through every sample',
        counts.ch.lineTo >= 115, counts.ch.lineTo);
  check('bed line is drawn through every sample',
        counts.bed.lineTo >= 115, counts.bed.lineTo);
  // Regression: the canvas must be cleared on every frame, not only when its
  // backing size changes. Without it the bands stack to solid and each step of
  // the time window leaves its labels and curve behind as ghosts.
  check('every draw starts from a cleared surface',
        counts.ch.clear === 1 && counts.bed.clear === 1,
        'chamber ' + counts.ch.clear + ', bed ' + counts.bed.clear);
  check('phase bands are merged into 3 runs per plot',
        counts.ch.rects.bands === 3 && counts.bed.rects.bands === 3,
        counts.ch.rects.bands + ' / ' + counts.bed.rects.bands);
  check('the relay strip marks the cooling half-cycles',
        counts.ch.rects.strips > 0 && counts.bed.rects.strips === 0,
        counts.ch.rects.strips + ' / ' + counts.bed.rects.strips);
  check('axis labels are rendered', counts.ch.text >= 8, counts.ch.text);

  // The chamber axis is snapped to a round tick step, so the labels are
  // multiples of a readable step rather than the old span/4 values
  // (28.4, 30.9, 33.4, ...).
  const chLabels = element('calplotch').getContext().of('fillText')
    .map((c) => c.args[0]).filter((v) => /^-?\d+(\.\d+)?$/.test(v)).map(Number);
  const round = [0.5, 1, 2, 2.5, 5, 10, 20, 50];
  let stepped = chLabels.length >= 5;
  for (let i = 1; i < chLabels.length; i++) {
    const d = Math.round((chLabels[i] - chLabels[i - 1]) * 10) / 10;
    if (round.indexOf(d) < 0) { stepped = false; }
  }
  check('chamber axis uses a round tick step', stepped, chLabels.join(' '));
  check('the waiting message is hidden', element('calplotmsg').style.display === 'none',
        element('calplotmsg').style.display);

  // 3. A gap in the middle: exactly one extra moveTo, and no line to the
  //    sentinel value.
  const withGap = makeTrace(120);
  withGap.ch[60] = -32768;
  sandbox.calTrace = withGap;
  const gapCounts = drawCounts(() => sandbox.drawTrace(sandbox.lastStatus));
  check('an invalid sample breaks the chamber line instead of drawing it',
        gapCounts.ch.moveTo === counts.ch.moveTo + 1,
        gapCounts.ch.moveTo + ' vs ' + counts.ch.moveTo);
  const yValues = element('calplotch').getContext().of('lineTo').map((c) => c.args[1]);
  check('no point is drawn at the sentinel temperature',
        yValues.every((y) => y > 0 && y < 124), JSON.stringify(yValues.slice(0, 4)));

  // 4. The live status point extends the line between trace fetches.
  sandbox.calTrace = makeTrace(120);
  sandbox.lastStatus = statusDoc({ active: true, elapsed: 700000 });
  const liveCounts = drawCounts(() => sandbox.drawTrace(sandbox.lastStatus));
  check('the live reading extends the chamber line',
        liveCounts.ch.lineTo === counts.ch.lineTo + 1,
        liveCounts.ch.lineTo + ' vs ' + counts.ch.lineTo);

  // 5. The server caps the series at 320 points; a capped series must draw.
  sandbox.lastStatus = statusDoc({ active: true, elapsed: 1595000 });
  sandbox.calTrace = makeTrace(320);
  counts = drawCounts(() => sandbox.drawTrace(sandbox.lastStatus));
  check('a 320-point (decimated) series draws in full',
        counts.ch.lineTo >= 315, counts.ch.lineTo);

  // 6. Fetch policy: starting a run drops the previous trace and fetches once;
  //    a status poll a second later must NOT refetch.
  net.calls.length = 0;
  net.trace = makeTrace(5);
  sandbox.calTrace = makeTrace(3); // the previous run's graph
  sandbox.calWasActive = false;
  let s = statusDoc({ active: true, phase: 1, elapsed: 1000 });
  sandbox.renderCal(s);
  check('starting a run drops the old trace immediately', sandbox.calTrace === null);
  await flush();
  check('starting a run fetches the trace', net.calls.length === 1, net.calls.length);
  check('the fetch result is kept', sandbox.calTrace && sandbox.calTrace.n === 5,
        sandbox.calTrace && sandbox.calTrace.n);

  net.calls.length = 0;
  sandbox.lastStatus = s;
  sandbox.renderCal(s);
  await flush();
  check('a 2 s status poll does not refetch the trace', net.calls.length === 0,
        net.calls.length);

  // 7. The run ends: one final fetch on the falling edge, and the chart moves
  //    into the result panel so the oscillation is visible next to the gains.
  net.calls.length = 0;
  sandbox.lastStatus = statusDoc({ hasResult: true });
  sandbox.renderCal(sandbox.lastStatus);
  await flush();
  check('the falling edge fetches the final samples', net.calls.length === 1,
        net.calls.length);
  check('the chart moves into the result dialog',
        element('calchart').parentNode && element('calchart').parentNode.id === 'calreschart',
        element('calchart').parentNode && element('calchart').parentNode.id);

  // 8. Applying the result returns the chart to the progress slot, so the next
  //    run draws into a panel that is actually on screen.
  sandbox.lastStatus = statusDoc({});
  sandbox.renderCal(sandbox.lastStatus);
  check('the chart returns to the progress slot',
        element('calchart').parentNode.id === 'calchartslot',
        element('calchart').parentNode.id);

  // 9. Elapsed time reads like the plugin tab: plain seconds for the first
  //    minute, then minutes and hours.
  check('elapsed 0 s is "0 s"', sandbox.formatElapsed(0) === '0 s', sandbox.formatElapsed(0));
  check('elapsed 42 s is "42 s"', sandbox.formatElapsed(42000) === '42 s', sandbox.formatElapsed(42000));
  check('elapsed 59 s is "59 s"', sandbox.formatElapsed(59000) === '59 s', sandbox.formatElapsed(59000));
  check('elapsed 60 s flips to "1m 00s"', sandbox.formatElapsed(60000) === '1m 00s', sandbox.formatElapsed(60000));
  check('elapsed 65 s is "1m 05s"', sandbox.formatElapsed(65000) === '1m 05s', sandbox.formatElapsed(65000));
  check('elapsed past an hour is "1h 02m 08s"',
        sandbox.formatElapsed(3728000) === '1h 02m 08s', sandbox.formatElapsed(3728000));
  sandbox.lastStatus = statusDoc({ active: true, phase: 1, elapsed: 42000 });
  sandbox.renderCal(sandbox.lastStatus);
  check('the running view prints 42 s in the first minute',
        element('calptime').textContent === '42 s', element('calptime').textContent);
  sandbox.lastStatus = statusDoc({ aborted: true, phase: 5, elapsed: 728000 });
  sandbox.renderCal(sandbox.lastStatus);
  check('the stopped view prints 12m 08s',
        element('calptime').textContent === '12m 08s', element('calptime').textContent);
  sandbox.lastStatus = statusDoc({ active: true, phase: 3, elapsed: 95000 });
  sandbox.renderCal(sandbox.lastStatus);
  check('the running view prints 1m 35s',
        element('calptime').textContent === '1m 35s', element('calptime').textContent);

  console.log('');
  if (failures) {
    console.log(failures + ' of ' + checks + ' checks FAILED');
    process.exit(1);
  }
  console.log('all ' + checks + ' checks passed');
})();
