/*
 * Fan Control tab for the EnclosureFanControl (C3) board.
 *
 * Target is OctoPrint 1.11, so the front end is the 1.x stack: Knockout for the
 * template bindings. The plugin contributes one Knockout view model and lets
 * OctoPrint's template system place the navbar entry and the tab content, which
 * is the smallest thing that works on this version.
 *
 * The view model never talks to the board. Every board interaction goes through
 * the plugin's own /api/plugin/enclosure_fan/* endpoints, so the C3 token stays on
 * the server and OctoPrint's app-key auth covers the browser-to-server hop. The
 * tab reads a server-side cache, which is why it stays responsive when the board
 * is unreachable instead of spinning.
 */
$(function () {
    var PLUGIN = "enclosure_fan";
    var POLL_MS = 1000;

    // Board enum -> readable text. Kept here rather than in Python because the tab
    // only ever displays them; the plugin passes the raw numbers through untouched.
    var STATE_NAMES = { 0: "Idle", 1: "PLA cooling", 2: "PETG cooling", 3: "ASA cooling" };
    var MODE_NAMES = { 0: "Automatic", 1: "Manual" };
    var MATERIAL_NAMES = { 0: "PLA", 1: "PETG", 2: "ASA", 3: "TPU", 4: "Unknown" };

    function statusText(map, value, fallback) {
        var key = parseInt(value, 10);
        return (key in map) ? map[key] : (fallback || String(value));
    }

    // "1d 04h", "12m 03s" - same coarse units as the CYD info screen, so the two
    // read the same way.
    function formatUptime(seconds) {
        seconds = parseInt(seconds, 10) || 0;
        if (seconds >= 86400) return Math.floor(seconds / 86400) + "d " + ("0" + Math.floor((seconds % 86400) / 3600)).slice(-2) + "h";
        if (seconds >= 3600) return Math.floor(seconds / 3600) + "h " + ("0" + Math.floor((seconds % 3600) / 60)).slice(-2) + "m";
        return Math.floor(seconds / 60) + "m " + ("0" + (seconds % 60)).slice(-2) + "s";
    }

    function EnclosureFanViewModel(parameters) {
        var self = this;

        self.settings = parameters[0];       // settingsViewModel
        self.loginState = parameters[1];     // loginStateViewModel

        // Observable state. Everything the template binds to.
        self.connected = ko.observable(false);
        self.errorText = ko.observable("");
        self.writeError = ko.observable("");

        self.chamber = ko.observable(null);
        self.chamberValid = ko.observable(false);
        self.chamberText = ko.pureComputed(function () {
            var c = self.chamber();
            return c === null ? "--" : (c / 10).toFixed(1);
        });
        self.setpoint = ko.observable("--");
        self.material = ko.observable("");
        self.stateText = ko.observable("");
        self.modeText = ko.observable("");
        self.jobText = ko.observable("");
        self.emergency = ko.observable(false);
        self.cydLinked = ko.observable(false);

        self.louvre = ko.observable(0);
        self.fan = ko.observable(0);
        self.louvreWidth = ko.pureComputed(function () { return { width: self.louvre() + "%" }; });
        self.fanWidth = ko.pureComputed(function () { return { width: self.fan() + "%" }; });

        self.ip = ko.observable("");
        self.fw = ko.observable("");
        self.rssi = ko.observable("");
        self.heapKb = ko.observable("");
        self.uptime = ko.observable("");

        // PID. pidMin/pidMax come from the BOARD (pid.min / pid.max in the status
        // document), so the bounds shown here are the ones it will actually accept
        // rather than a copy that can drift from enclosure_pid.h.
        self.kp = ko.observable(0);
        self.ki = ko.observable(0);
        self.kd = ko.observable(0);
        self.pidMin = ko.observable({ kp: 0, ki: 0, kd: 0 });
        self.pidMax = ko.observable({ kp: 50, ki: 5, kd: 50 });
        self.pidSaved = ko.observable(true);
        self.pidSaving = ko.observable(false);

        self.pidRange = function (which) {
            var lo = self.pidMin()[which], hi = self.pidMax()[which];
            return lo + " to " + hi;
        };

        // Manual control.
        self.manualMode = ko.observable(false);
        self.manualLouvre = ko.observable(0);
        self.manualFan = ko.observable(0);
        self.manualBusy = ko.observable(false);

        // ------------------------------------------------------------------ //
        // Polling
        // ------------------------------------------------------------------ //

        var timer = null;

        // Reads the plugin's cache. Never rejects on a network failure - that is
        // the whole point of the cache - so the only error path is a malformed
        // response, which is treated as "not connected" rather than thrown.
        function poll() {
            OctoPrint.simpleApiCommand(PLUGIN, "status", {}, function (response) {
                var payload = response && response.response ? response.response : response;
                if (!payload) {
                    self.connected(false);
                    self.errorText("No response from OctoPrint.");
                    return;
                }
                apply(payload);
            });
        }

        function apply(payload) {
            self.connected(!!payload.ok);
            if (!payload.ok) {
                self.errorText(payload.error || "The enclosure board is not responding.");
                return;
            }
            self.errorText("");

            var s = payload.status;
            if (!s) return;

            self.chamber(s.chamber === undefined ? null : s.chamber);
            self.chamberValid(!!s.chamberValid);
            self.setpoint(s.setpoint === undefined ? "--" : s.setpoint);
            self.material(statusText(MATERIAL_NAMES, s.material, "Unknown"));
            self.stateText(statusText(STATE_NAMES, s.state, "Unknown"));
            self.modeText(statusText(MODE_NAMES, s.mode, "Unknown"));
            self.jobText(s.job ? "printing" : "idle");
            self.emergency(!!s.emergency);
            self.cydLinked(!!s.remote);

            self.louvre(s.louvre || 0);
            self.fan(s.fan || 0);

            self.ip(s.ip || "");
            self.fw(s.fw || "");
            self.rssi(s.rssi === undefined ? "" : s.rssi);
            self.heapKb(s.heap === undefined ? "" : Math.round(s.heap / 1024));
            self.uptime(formatUptime(s.uptime));

            var pid = s.pid || {};
            // Only overwrite the inputs when the user is not mid-edit. A 1 Hz poll
            // that rewrites the fields while someone is typing in kp would make the
            // box unusable - this is the classic reason live forms need a dirty flag.
            if (!self.pidSaving() && !self.pidDirty()) {
                self.kp(pid.kp);
                self.ki(pid.ki);
                self.kd(pid.kd);
            }
            self.pidMin(pid.min || { kp: 0, ki: 0, kd: 0 });
            self.pidMax(pid.max || { kp: 50, ki: 5, kd: 50 });
            // pidSaved is a TOP-LEVEL field, not inside the pid object.
            self.pidSaved(s.pidSaved === undefined ? true : !!s.pidSaved);

            // Seed the manual sliders from what the board actually has, but only
            // while they are untouched - otherwise a poll would yank the slider
            // back mid-drag.
            if (!self.manualBusy() && !self.manualDirty()) {
                self.manualLouvre(s.manualLouvre || 0);
                self.manualFan(s.manualFan || 0);
            }
            self.manualMode(parseInt(s.mode, 10) === 1);
        }

        // Dirty flags. Set when the user types or drags, cleared once the write
        // they were making has been sent and answered.
        self.pidDirty = ko.observable(false);
        self.manualDirty = ko.observable(false);

        [self.kp, self.ki, self.kd].forEach(function (obs) {
            obs.subscribe(function () { self.pidDirty(true); });
        });
        [self.manualLouvre, self.manualFan].forEach(function (obs) {
            obs.subscribe(function () { self.manualDirty(true); });
        });

        self.onBeforeUnload = function () {
            if (timer !== null) {
                clearInterval(timer);
                timer = null;
            }
        };

        function startPolling() {
            if (timer === null) {
                poll();
                timer = setInterval(poll, POLL_MS);
            }
        }

        // Poll continuously - including while on another tab - so the numbers are
        // already current when the user switches back. 1 Hz against the plugin's
        // own cache costs nothing on the board.
        startPolling();
        $(window).on("beforeunload", self.onBeforeUnload);

        // ------------------------------------------------------------------ //
        // Writes
        // ------------------------------------------------------------------ //

        function fail(response) {
            var text = "The enclosure board did not accept that.";
            try {
                var payload = response && response.response ? response.response : response;
                if (payload && payload.error) text = payload.error;
            } catch (e) { /* fall through to the generic message */ }
            self.writeError(text);
            PNotify.notify(text, "error");
        }

        self.applyPid = function () {
            var kp = parseFloat(self.kp()), ki = parseFloat(self.ki()), kd = parseFloat(self.kd());
            if (isNaN(kp) || isNaN(ki) || isNaN(kd)) {
                self.writeError("kp, ki and kd must all be numbers.");
                return;
            }
            self.pidSaving(true);
            OctoPrint.simpleApiCommand(PLUGIN, "setpid", { kp: kp, ki: ki, kd: kd }, function (response) {
                self.pidSaving(false);
                self.writeError("");
                self.pidDirty(false);
                if (response && response.error) { fail(response); return; }
                var payload = response && response.response ? response.response : response;
                if (payload && payload.status) apply(payload);
                PNotify.notify("PID gains applied", "success");
            });
        };

        self.savePid = function () {
            self.pidSaving(true);
            OctoPrint.simpleApiCommand(PLUGIN, "savepid", {}, function (response) {
                self.pidSaving(false);
                self.writeError("");
                self.pidDirty(false);
                if (response && response.error) { fail(response); return; }
                var payload = response && response.response ? response.response : response;
                if (payload && payload.status) apply(payload);
                PNotify.notify("PID gains saved to the board", "success");
            });
        };

        self.defaultPid = function () {
            self.pidSaving(true);
            OctoPrint.simpleApiCommand(PLUGIN, "defaultpid", {}, function (response) {
                self.pidSaving(false);
                self.writeError("");
                self.pidDirty(false);
                if (response && response.error) { fail(response); return; }
                var payload = response && response.response ? response.response : response;
                if (payload && payload.status) apply(payload);
                PNotify.notify("PID gains reset to the board's defaults", "success");
            });
        };

        // Mode and the two outputs always travel together: the board's /api/manual
        // takes mode, louvre and fan in one request, so there is no such thing as
        // changing the mode without also restating the outputs.
        function sendManual(mode, louvre, fan) {
            self.manualBusy(true);
            OctoPrint.simpleApiCommand(PLUGIN, "setmanual",
                { mode: mode, louvre: louvre, fan: fan }, function (response) {
                    self.manualBusy(false);
                    self.writeError("");
                    self.manualDirty(false);
                    if (response && response.error) { fail(response); return; }
                    var payload = response && response.response ? response.response : response;
                    if (payload && payload.status) apply(payload);
                });
        }

        self.setAuto = function () { sendManual(0, 0, 0); };
        self.setManual = function () {
            sendManual(1, parseInt(self.manualLouvre(), 10), parseInt(self.manualFan(), 10));
        };
        self.pushManual = function () {
            if (!self.manualMode()) return;
            sendManual(1, parseInt(self.manualLouvre(), 10), parseInt(self.manualFan(), 10));
        };
    }

    // ---------------------------------------------------------------------- //
    // Wiring: the Knockout view model.
    // ---------------------------------------------------------------------- //

    // The navbar entry and the tab content are NOT hand-built here. OctoPrint's
    // template system renders both from get_template_configs: the "tab" entry
    // produces <li id="tab_plugin_enclosure_fan_link"><a href="#tab_plugin_enclosure_fan"
    // data-toggle="tab">, and the "settings" entry produces the settings-dialog
    // block. Injecting a second navbar <a> from JS duplicated that with an element
    // OctoPrint's tab tracking does not know about.
    //
    // `elements` is what actually binds the view model to the DOM: main.js takes
    // viewModel.elements as its bind targets, so omitting it constructs the view
    // model and then attaches it to nothing - a blank tab.
    $(document).on("DOMReady", function () {
        OCTOPRINT_VIEWMODELS.push({
            construct: EnclosureFanViewModel,
            dependencies: ["settingsViewModel", "loginStateViewModel"],
            elements: ["#tab_plugin_enclosure_fan", "#settings_plugin_enclosure_fan"]
        });
    });
});