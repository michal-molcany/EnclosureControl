/*
 * Front-end smoke test for the EnclosureFan plugin's view model.
 *
 * The Python selftest can only check that the page CONTAINS the right names.
 * This one actually RUNS the shipped JavaScript: it stubs just enough of
 * Knockout, jQuery and OctoPrint to construct the real view model from the
 * real asset file and drive a whole calibration through it - open the
 * confirmation, refuse to start without the acknowledgement, start, watch the
 * live values, cancel, read the board's reason, dismiss, and apply.
 *
 * It needs Node (node --version) and nothing else: no npm install, no
 * knockout package. The stubs below are deliberately small; if the view model
 * grows a dependency on something they do not provide, this test fails rather
 * than silently passing.
 *
 * Run:  node tools/selftest_viewmodel.js
 */

const fs = require("fs");
const path = require("path");
const nodevm = require("vm");

const ROOT = path.dirname(__dirname);
const JS = path.join(ROOT, "octoprint_enclosure_fan", "static", "js",
                     "enclosure_fan.js");

// --------------------------------------------------------------------------
// Knockout stub. Observables notify subscribers; computed values are
// evaluated on every read, which is slower but always correct.
// --------------------------------------------------------------------------

function observable(initial) {
    let value = initial;
    const subscribers = [];
    const o = function (v) {
        if (arguments.length) {
            value = v;
            subscribers.forEach((fn) => fn(value));
            return;
        }
        return value;
    };
    o.subscribe = function (fn) { subscribers.push(fn); };
    return o;
}

const ko = {
    observable: observable,
    pureComputed: function (fn) { return function () { return fn(); }; }
};

// --------------------------------------------------------------------------
// Promise stub. OctoPrint.simpleApiCommand returns a jQuery promise, and the
// plugin only ever uses .done/.fail/.always on it.
// --------------------------------------------------------------------------

function resolved(value) {
    const promise = {
        done(fn) { fn(value); return promise; },
        fail() { return promise; },
        always(fn) { fn(); return promise; }
    };
    return promise;
}

// --------------------------------------------------------------------------
// The board. One status document, mutated by the calibration actions, so the
// test drives the same state machine the C3 does.
// --------------------------------------------------------------------------

let status = {
    ok: true, fw: "0.5.7", ip: "10.0.0.1", rssi: -60, heap: 190000,
    uptime: 900, chamber: 267, chamberValid: true, bed: 251, bedValid: true,
    setpoint: 35, material: 0, state: 0, mode: 0, job: false, emergency: false,
    remote: true, louvre: 0, fan: 0, manualLouvre: 0, manualFan: 0,
    pidSaved: true,
    pid: { kp: 50, ki: 0.466823, kd: 50,
           min: { kp: 0, ki: 0, kd: 0 }, max: { kp: 50, ki: 5, kd: 50 } },
    cal: { active: false, hasResult: false, aborted: false, phase: 0,
           elapsed: 0, cycles: 0, bedTarget: 90, setpoint: 35, error: "",
           oldKp: 50, oldKi: 0.466823, oldKd: 50, kp: 0, ki: 0, kd: 0,
           ku: 0, tu: 0, amp: 0, delta: 0, clamped: false,
           zcSym: true, ampConv: true }
};

const actions = [];

const OctoPrint = {
    simpleApiCommand(plugin, name, payload) {
        if (name === "calibrate") {
            actions.push(payload.action);
            switch (payload.action) {
            case "start":
                status.cal.active = true;
                status.cal.phase = 1;
                status.cal.elapsed = 65000;
                status.cal.cycles = 3;
                break;
            case "cancel":
                status.cal.active = false;
                status.cal.aborted = true;
                status.cal.phase = 5;
                status.cal.error = "cancelled by the user";
                break;
            case "apply":
                status.cal.hasResult = false;
                status.cal.aborted = false;
                status.pid = { kp: 12.5, ki: 0.4, kd: 3,
                               min: { kp: 0, ki: 0, kd: 0 },
                               max: { kp: 50, ki: 5, kd: 50 } };
                break;
            case "discard":
                status.cal.hasResult = false;
                status.cal.aborted = false;
                status.cal.error = "";
                break;
            default:
                break;
            }
        }
        return resolved({ ok: true, status: status, error: null });
    }
};

function $(arg) {
    if (typeof arg === "function") { arg(); return; }
    return { on() { return this; } };
}

const sandbox = {
    ko: ko, OctoPrint: OctoPrint, $: $, window: {},
    PNotify: function () {},
    OCTOPRINT_VIEWMODELS: [],
    setInterval: setInterval, clearInterval: clearInterval,
    setTimeout: setTimeout, console: console
};
sandbox.globalThis = sandbox;
nodevm.createContext(sandbox);
nodevm.runInContext(fs.readFileSync(JS, "utf8"), sandbox, { filename: JS });

// --------------------------------------------------------------------------
// The checks.
// --------------------------------------------------------------------------

let failures = 0;

function check(name, condition, detail) {
    const ok = !!condition;
    console.log((ok ? "  PASS  " : "  FAIL  ") + name +
                (!ok && detail !== undefined ? "  -> " + detail : ""));
    if (!ok) { failures += 1; }
}

const Ctor = sandbox.OCTOPRINT_VIEWMODELS[0].construct;
const view = new Ctor([{}, {}]);

check("calibration is reported as supported", view.calSupported() === true);
check("the PID fields followed the board", view.kp() === 50, view.kp());
check("the form is not dirty after a refresh", view.pidDirty() === false);
check("calibrate is enabled while idle", view.calStartDisabled() === false);

view.openCalibrationConfirm();
check("the confirmation opens", view.calConfirm() === true);
check("calibrate is disabled while confirming",
      view.calStartDisabled() === true);
view.startCalibration();
check("start is refused without the acknowledgement", actions.length === 0,
      actions.join(","));

view.calAck(true);
view.startCalibration();
check("start reached the board", actions[actions.length - 1] === "start");
check("the run shows as active", view.calActive() === true);
check("the confirmation closed", view.calConfirm() === false);
check("the phase is named", view.calPhaseText() === "preheating bed",
      view.calPhaseText());
check("elapsed is formatted", view.calElapsedText() === "1m 05s",
      view.calElapsedText());
check("the bed shows its target",
      view.calBedText() === "25.1 \u00b0C (target 90.0 \u00b0C)",
      view.calBedText());
check("the chamber is shown", view.calChamberText() === "26.7 \u00b0C",
      view.calChamberText());
check("the PID inputs are locked during a run",
      view.pidControlsDisabled() === true);
check("manual control is locked during a run",
      view.manualControlsDisabled() === true);
check("the badge shows the phase", view.calBadgeText() === "preheating bed",
      view.calBadgeText());

view.cancelCalibration();
check("cancel reached the board", actions[actions.length - 1] === "cancel");
check("the run shows as stopped", view.calAborted() === true);
check("the board's reason is shown",
      view.calErrorText() === "cancelled by the user", view.calErrorText());
check("calibrate stays disabled until dismissed",
      view.calStartDisabled() === true);

view.dismissCalibration();
check("dismiss reached the board", actions[actions.length - 1] === "discard");
check("the stopped state is cleared", view.calAborted() === false);

// A finished run: the result, then apply.
status.cal = { active: false, hasResult: true, aborted: false, phase: 4,
                elapsed: 120000, cycles: 6, bedTarget: 90, setpoint: 35,
                error: "", oldKp: 50, oldKi: 0.466823, oldKd: 50,
                kp: 12.5, ki: 0.4, kd: 3, ku: 1.5, tu: 22.4, amp: 0.8,
                delta: 12, clamped: false, zcSym: false, ampConv: true };
view.applyCalibration();
check("apply reached the board", actions[actions.length - 1] === "apply");
check("the result is cleared after applying", view.calHasResult() === false);
check("the PID inputs show the new gains", view.kp() === 12.5, view.kp());
check("the PID inputs are editable again", view.pidControlsDisabled() === false);

console.log("");
if (failures) {
    console.log("FAILURES: " + failures);
    process.exit(1);
}
console.log("all checks passed");
process.exit(0);
