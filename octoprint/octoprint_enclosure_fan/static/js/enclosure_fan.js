/*
 * Fan Control tab and settings for the EnclosureFanControl (C3) board.
 *
 * Target is OctoPrint 1.11, so the front end is the 1.x stack: Knockout for the
 * template bindings. The plugin contributes one Knockout view model and lets
 * OctoPrint's template system place the navbar entry and the tab content, which
 * is the smallest thing that works on this version.
 *
 * The view model never talks to the board. Every board interaction goes through
 * the plugin's own /api/plugin/enclosure_fan endpoint, so the C3 token stays on
 * the server and OctoPrint's app-key auth covers the browser-to-server hop. The
 * tab reads a server-side cache, which is why it stays responsive when the board
 * is unreachable instead of spinning.
 *
 * TWO API DETAILS THAT COST A DAY BETWEEN THEM - do not "simplify" either:
 *
 *   1. OctoPrint.simpleApiCommand(...) returns a jQuery promise. It does NOT
 *      take a callback: the fourth argument is an options object, and a function
 *      passed there is silently dropped by $.extend. Calling it with a callback
 *      sends the request, ignores the answer, and leaves the UI on its initial
 *      values forever. Everything below is .done()/.fail().
 *
 *   2. The dependency injected as parameters[0] must be exposed on the view
 *      model under the name the TEMPLATE uses. Plugin templates are bound with
 *      the plugin view model as Knockout's root, so `settings.plugins...` would
 *      look for `plugins` on the plugin view model - not on the settings view
 *      model - and throw. Hence `settingsViewModel`, matching
 *      settingsViewModel.settings.plugins.enclosure_fan.* in both templates.
 */

$(function () {
    var PLUGIN = "enclosure_fan";
    var POLL_MS = 1000;

    // Board enum -> readable text. Kept here rather than in Python because the tab
    // only ever displays them; the plugin passes the raw numbers through untouched.
    // The lists mirror lib/EnclosureProto/src/enclosure_proto.h; a value missing
    // here would otherwise render as a bare number ("7" for unknown material).
    var STATE_NAMES = { 0: "Idle", 1: "PLA cooling", 2: "PETG cooling", 3: "ASA cooling" };
    var MODE_NAMES = { 0: "Automatic", 1: "Manual" };
    var MATERIAL_NAMES = {
        0: "PLA",
        1: "PETG",
        2: "ASA",
        3: "ABS",
        4: "TPU",
        5: "Nylon",
        6: "High-temp",
        7: "Unknown"
    };

    // Mirrors EnclosureCalPhase in lib/EnclosureProto/src/enclosure_proto.h.
    var CAL_PHASES = ["", "preheating bed", "settling", "relay autotune",
                      "done", "aborted"];

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

    // The board sends a number for every live measurement, but a missing field
    // must render as "--" rather than "NaN". Keep the distinction between 0 and
    // "not present": 0 is a perfectly valid cycle count, gain or temperature.
    function optionalNumber(value) {
        return (typeof value === "number" && isFinite(value)) ? value : null;
    }

    function formatNumber(value, digits) {
        var number = optionalNumber(value);
        return number === null ? "--" : number.toFixed(digits);
    }

    function formatWithUnit(value, digits, unit) {
        var number = optionalNumber(value);
        return number === null ? "--" : number.toFixed(digits) + " " + unit;
    }

    function formatElapsed(milliseconds) {
        var total = Math.floor((parseInt(milliseconds, 10) || 0) / 1000);
        var hours = Math.floor(total / 3600);
        var minutes = Math.floor((total % 3600) / 60);
        var seconds = total % 60;
        if (hours) {
            return hours + "h " + ("0" + minutes).slice(-2) + "m " +
                ("0" + seconds).slice(-2) + "s";
        }
        return minutes + "m " + ("0" + seconds).slice(-2) + "s";
    }

    // One command, always the same shape: a promise whose done() gets the parsed
    // JSON body and whose fail() gets the jqXHR. Both the tab and the settings
    // page go through here so the two cannot drift apart again.
    function command(name, payload) {
        return OctoPrint.simpleApiCommand(PLUGIN, name, payload || {});
    }

    // The plugin answers a refused command with HTTP 400 and {error: "..."};
    // jQuery rejects, so the message lives in responseJSON, not in the body.
    function failureText(xhr, fallback) {
        if (xhr && xhr.responseJSON && xhr.responseJSON.error) {
            return xhr.responseJSON.error;
        }
        if (xhr && xhr.status === 0) {
            return "Could not reach OctoPrint - the connection dropped.";
        }
        if (xhr && xhr.status === 403) {
            return "OctoPrint refused the request. Log in again and reload the page.";
        }
        if (xhr && xhr.status) {
            return "OctoPrint answered HTTP " + xhr.status +
                (xhr.statusText ? " (" + xhr.statusText + ")" : "") + ".";
        }
        return fallback || "The request did not complete.";
    }

    function notify(text, type) {
        // OctoPrint 1.11 ships PNotify 4 with the constructor form. PNotify.notify
        // does not exist on it, and calling it would throw inside the success
        // handler - after the write had already been applied on the board.
        new PNotify({
            title: "Enclosure fan control",
            text: text,
            type: type
        });
    }

    function EnclosureFanViewModel(parameters) {
        var self = this;

        // True only while the code itself writes an observable that a user edit
        // also writes. Without it, refreshing the PID inputs from the board
        // would look like a user edit and mark the form dirty - and a dirty form
        // is never refreshed again, so the fields would freeze at whatever they
        // showed when the board last changed. That is visible with a
        // calibration result: the new gains are applied on the board, but the
        // inputs would keep showing the old ones.
        var suppressDirty = false;

        // Named for the template binding, not for taste - see the header note.
        self.settingsViewModel = parameters[0];
        self.loginState = parameters[1];

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
        self.bed = ko.observable(null);
        self.bedValid = ko.observable(false);
        self.bedText = ko.pureComputed(function () {
            var b = self.bed();
            return b === null ? "--" : (b / 10).toFixed(1);
        });
        self.setpoint = ko.observable("--");
        self.material = ko.observable("");
        self.stateText = ko.observable("");
        self.modeText = ko.observable("");
        self.jobText = ko.observable("");
        self.jobActive = ko.observable(false);
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

        // PID calibration. Raw values are kept as observables and formatted in
        // computeds, so a missing field can render as "--" without mistaking a
        // legitimate 0 (cycles, gain) for "no data".
        self.calSupported = ko.observable(false);
        self.calActive = ko.observable(false);
        self.calHasResult = ko.observable(false);
        self.calAborted = ko.observable(false);
        self.calPhase = ko.observable(0);
        self.calElapsed = ko.observable(0);
        self.calCycles = ko.observable(0);
        self.calBedTarget = ko.observable(null);
        self.calBoardError = ko.observable("");
        self.calError = ko.observable("");
        self.calBusy = ko.observable(false);
        self.calConfirm = ko.observable(false);
        self.calAck = ko.observable(false);
        self.calOldKp = ko.observable(null);
        self.calOldKi = ko.observable(null);
        self.calOldKd = ko.observable(null);
        self.calKp = ko.observable(null);
        self.calKi = ko.observable(null);
        self.calKd = ko.observable(null);
        self.calKu = ko.observable(null);
        self.calTu = ko.observable(null);
        self.calAmp = ko.observable(null);
        self.calDelta = ko.observable(null);
        self.calClamped = ko.observable(false);
        self.calZcSym = ko.observable(true);
        self.calAmpConv = ko.observable(true);

        // Manual control.
        self.manualMode = ko.observable(false);
        self.manualLouvre = ko.observable(0);
        self.manualFan = ko.observable(0);
        self.manualBusy = ko.observable(false);

        // Derived calibration display state.
        self.calPhaseText = ko.pureComputed(function () {
            return CAL_PHASES[self.calPhase()] || "running";
        });
        self.calElapsedText = ko.pureComputed(function () {
            return formatElapsed(self.calElapsed());
        });
        self.calChamberText = ko.pureComputed(function () {
            return self.chamberValid() ? self.chamberText() + " \u00b0C" : "no data";
        });
        self.calBedText = ko.pureComputed(function () {
            if (!self.bedValid()) return "no data";
            var target = optionalNumber(self.calBedTarget());
            if (target === null) return self.bedText() + " \u00b0C";
            return self.bedText() + " \u00b0C (target " +
                target.toFixed(1) + " \u00b0C)";
        });
        self.calBadgeText = ko.pureComputed(function () {
            if (self.calActive()) return self.calPhaseText();
            if (self.calHasResult()) return "result ready";
            if (self.calAborted()) return "stopped";
            return "idle";
        });
        self.calBadgeClass = ko.pureComputed(function () {
            if (self.calActive()) return "label label-warning";
            if (self.calHasResult()) return "label label-success";
            if (self.calAborted()) return "label label-important";
            return "label";
        });
        self.calErrorText = ko.pureComputed(function () {
            return self.calError() || self.calBoardError() ||
                "The calibration did not finish.";
        });
        self.calWarnings = ko.pureComputed(function () {
            var warnings = [];
            if (self.calClamped()) {
                warnings.push("One or more gains were clamped to the accepted range (kp <= 50, ki <= 5, kd <= 50).");
            }
            if (!self.calZcSym()) {
                warnings.push("The oscillation was not symmetrical; the result may be inaccurate.");
            }
            if (!self.calAmpConv()) {
                warnings.push("The oscillation had not fully settled; the result may be inaccurate.");
            }
            return warnings.join(" ");
        });
        self.calGainRows = ko.pureComputed(function () {
            return [
                { name: "kp", oldText: formatNumber(self.calOldKp(), 3),
                  newText: formatNumber(self.calKp(), 3) },
                { name: "ki", oldText: formatNumber(self.calOldKi(), 3),
                  newText: formatNumber(self.calKi(), 3) },
                { name: "kd", oldText: formatNumber(self.calOldKd(), 3),
                  newText: formatNumber(self.calKd(), 3) }
            ];
        });
        self.calMetrics = ko.pureComputed(function () {
            return [
                { name: "Ku", text: formatWithUnit(self.calKu(), 2, "output/\u00b0C") },
                { name: "Tu", text: formatWithUnit(self.calTu(), 1, "s") },
                { name: "Amplitude", text: formatWithUnit(self.calAmp(), 2, "\u00b0C") },
                { name: "Relay half-span (d)",
                  text: formatWithUnit(self.calDelta(), 1, "output units") }
            ];
        });
        self.calStartDisabled = ko.pureComputed(function () {
            return !self.connected() || !self.calSupported() ||
                self.calActive() || self.calHasResult() || self.calAborted() ||
                self.jobActive() || self.calBusy() || self.calConfirm();
        });
        self.pidControlsDisabled = ko.pureComputed(function () {
            return self.pidSaving() || self.calActive() || self.calHasResult();
        });
        self.manualControlsDisabled = ko.pureComputed(function () {
            return self.manualBusy() || self.calActive();
        });

        // Settings-page connection test.
        self.testBusy = ko.observable(false);
        self.testOk = ko.observable(false);
        self.testResult = ko.observable("");

        // ------------------------------------------------------------------ //
        // Polling
        // ------------------------------------------------------------------ //

        var timer = null;

        // Reads the plugin's cache. A network failure here means OctoPrint itself
        // is unreachable, not the board - the board's state arrives as ok=false
        // inside the payload.
        function poll() {
            command("status").done(function (payload) {
                apply(payload || { ok: false, error: "OctoPrint returned an empty answer." });
            }).fail(function (xhr) {
                self.connected(false);
                self.errorText(failureText(xhr, "No response from OctoPrint."));
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
            self.bed(s.bed === undefined ? null : s.bed);
            self.bedValid(!!s.bedValid);
            self.setpoint(s.setpoint === undefined ? "--" : s.setpoint);
            self.material(statusText(MATERIAL_NAMES, s.material, "Unknown"));
            self.stateText(statusText(STATE_NAMES, s.state, "Unknown"));
            self.modeText(statusText(MODE_NAMES, s.mode, "Unknown"));
            self.jobActive(!!s.job);
            self.jobText(s.job ? "printing" : "idle");
            self.emergency(!!s.emergency);
            self.cydLinked(!!s.remote);

            // Calibration block. Its presence is also the firmware capability
            // check: older C3 builds do not report it at all.
            var cal = s.cal;
            self.calSupported(!!cal && typeof cal === "object");
            cal = cal || {};
            self.calActive(!!cal.active);
            self.calHasResult(!!cal.hasResult);
            self.calAborted(!!cal.aborted);
            self.calPhase(parseInt(cal.phase, 10) || 0);
            self.calElapsed(parseInt(cal.elapsed, 10) || 0);
            self.calCycles(parseInt(cal.cycles, 10) || 0);
            self.calBedTarget(optionalNumber(cal.bedTarget));
            self.calBoardError(cal.error || "");
            self.calOldKp(optionalNumber(cal.oldKp));
            self.calOldKi(optionalNumber(cal.oldKi));
            self.calOldKd(optionalNumber(cal.oldKd));
            self.calKp(optionalNumber(cal.kp));
            self.calKi(optionalNumber(cal.ki));
            self.calKd(optionalNumber(cal.kd));
            self.calKu(optionalNumber(cal.ku));
            self.calTu(optionalNumber(cal.tu));
            self.calAmp(optionalNumber(cal.amp));
            self.calDelta(optionalNumber(cal.delta));
            self.calClamped(!!cal.clamped);
            self.calZcSym(cal.zcSym === undefined ? true : !!cal.zcSym);
            self.calAmpConv(cal.ampConv === undefined ? true : !!cal.ampConv);

            // A run starting (from this tab, the C3 page or the CYD) invalidates
            // any half-typed gains and closes the local confirmation.
            if (self.calActive()) {
                self.pidDirty(false);
            }
            if (self.calActive() || self.calHasResult() || self.calAborted() ||
                    self.jobActive()) {
                self.calConfirm(false);
            }

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
                suppressDirty = true;
                self.kp(pid.kp);
                self.ki(pid.ki);
                self.kd(pid.kd);
                suppressDirty = false;
            }
            self.pidMin(pid.min || { kp: 0, ki: 0, kd: 0 });
            self.pidMax(pid.max || { kp: 50, ki: 5, kd: 50 });
            // pidSaved is a TOP-LEVEL field, not inside the pid object.
            self.pidSaved(s.pidSaved === undefined ? true : !!s.pidSaved);

            // Seed the manual sliders from what the board actually has, but only
            // while they are untouched - otherwise a poll would yank the slider
            // back mid-drag.
            if (!self.manualBusy() && !self.manualDirty()) {
                suppressDirty = true;
                self.manualLouvre(s.manualLouvre || 0);
                self.manualFan(s.manualFan || 0);
                suppressDirty = false;
            }
            self.manualMode(parseInt(s.mode, 10) === 1);
        }

        // Dirty flags. Set when the user types or drags, cleared once the write
        // they were making has been sent and answered.
        self.pidDirty = ko.observable(false);
        self.manualDirty = ko.observable(false);

        [self.kp, self.ki, self.kd].forEach(function (obs) {
            obs.subscribe(function () {
                if (!suppressDirty) self.pidDirty(true);
            });
        });
        [self.manualLouvre, self.manualFan].forEach(function (obs) {
            obs.subscribe(function () {
                if (!suppressDirty) self.manualDirty(true);
            });
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

        // Shared by every write: report the failure where the user is looking,
        // keep the message the server chose (the CYD gate's explanation is the
        // whole point of that 400), and refresh from the board's own answer.
        function reportFailure(xhr) {
            var text = failureText(xhr, "The enclosure board did not accept that.");
            self.writeError(text);
            notify(text, "error");
        }

        function write(name, payload, successText) {
            return command(name, payload).done(function (response) {
                self.writeError("");
                var body = response || {};
                if (body.status) apply(body);
                if (successText) notify(successText, "success");
            }).fail(reportFailure);
        }

        self.applyPid = function () {
            var kp = parseFloat(self.kp()), ki = parseFloat(self.ki()), kd = parseFloat(self.kd());
            if (isNaN(kp) || isNaN(ki) || isNaN(kd)) {
                self.writeError("kp, ki and kd must all be numbers.");
                return;
            }
            self.pidSaving(true);
            write("setpid", { kp: kp, ki: ki, kd: kd }, "PID gains applied")
                .always(function () {
                    self.pidSaving(false);
                    self.pidDirty(false);
                });
        };

        self.savePid = function () {
            self.pidSaving(true);
            write("savepid", {}, "PID gains saved to the board")
                .always(function () {
                    self.pidSaving(false);
                    self.pidDirty(false);
                });
        };

        self.defaultPid = function () {
            self.pidSaving(true);
            write("defaultpid", {}, "PID gains reset to the board's defaults")
                .always(function () {
                    self.pidSaving(false);
                    self.pidDirty(false);
                });
        };

        // ------------------------------------------------------------------ //
        // PID calibration
        // ------------------------------------------------------------------ //

        // Every calibration action returns the full status document, just like
        // the PID and manual writes, so apply() below refreshes the whole tab.
        function calibrationCommand(action, successText) {
            self.calBusy(true);
            self.calError("");
            return command("calibrate", { action: action }).done(function (response) {
                self.writeError("");
                var body = response || {};
                if (body.status) apply(body);
                if (successText) notify(successText, "success");
            }).fail(function (xhr) {
                var text = failureText(xhr, "The enclosure board did not accept that.");
                self.calError(text);
                self.writeError(text);
                notify(text, "error");
            }).always(function () {
                self.calBusy(false);
            });
        }

        self.openCalibrationConfirm = function () {
            if (self.calStartDisabled()) return;
            self.calAck(false);
            self.calError("");
            self.calConfirm(true);
        };

        self.closeCalibrationConfirm = function () {
            if (self.calBusy()) return;
            self.calConfirm(false);
            self.calAck(false);
        };

        self.startCalibration = function () {
            if (!self.calAck() || self.calBusy()) return;
            calibrationCommand("start", "PID calibration started").done(function () {
                // apply() also clears this when the status says active; doing it
                // here makes the panel disappear immediately on a good answer.
                self.calConfirm(false);
            });
        };

        self.cancelCalibration = function () {
            calibrationCommand("cancel", "Calibration cancellation requested");
        };

        self.dismissCalibration = function () {
            calibrationCommand("discard", "Calibration result dismissed");
        };

        self.discardCalibration = function () {
            calibrationCommand("discard", "Calibration gains discarded");
        };

        self.applyCalibration = function () {
            calibrationCommand("apply", "Calibration gains applied (RAM only)");
        };

        self.applyAndSaveCalibration = function () {
            // Mirrors the C3 page: apply consumes the result, then persist the
            // gains with the existing PID-save route. A save failure is reported
            // separately because by then the result dialog is gone.
            self.calBusy(true);
            self.calError("");
            command("calibrate", { action: "apply" }).done(function (response) {
                var body = response || {};
                if (body.status) apply(body);
                command("savepid", {}).done(function (saved) {
                    var savedBody = saved || {};
                    if (savedBody.status) apply(savedBody);
                    self.writeError("");
                    notify("Calibration gains applied and saved to NVS", "success");
                }).fail(function (xhr) {
                    var text = "Gains applied, but saving failed: " +
                        failureText(xhr, "the board did not save them.");
                    self.calError(text);
                    self.writeError(text);
                    notify(text, "error");
                }).always(function () {
                    self.calBusy(false);
                });
            }).fail(function (xhr) {
                var text = failureText(xhr, "The enclosure board did not accept that.");
                self.calError(text);
                self.writeError(text);
                notify(text, "error");
                self.calBusy(false);
            });
        };

        // Mode and the two outputs always travel together: the board's /api/manual
        // takes mode, louvre and fan in one request, so there is no such thing as
        // changing the mode without also restating the outputs.
        function sendManual(mode, louvre, fan) {
            self.manualBusy(true);
            write("setmanual", { mode: mode, louvre: louvre, fan: fan })
                .always(function () {
                    self.manualBusy(false);
                    self.manualDirty(false);
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

        // ------------------------------------------------------------------ //
        // Settings: test connection
        // ------------------------------------------------------------------ //

        function enteredSettings() {
            // Optional chaining is not used here: OctoPrint 1.11 still ships a
            // JavaScript toolchain that chokes on it in plugin assets.
            var svm = self.settingsViewModel;
            var settings = svm ? svm.settings : undefined;
            var plugins = settings ? settings.plugins : undefined;
            return plugins ? plugins.enclosure_fan : undefined;
        }

        self.testConnection = function () {
            var form = enteredSettings();
            var payload = {};

            if (form && form.host) payload.host = form.host() || "";
            if (form && form.port) payload.port = form.port();
            // An empty box means "keep the saved token": the server never sends a
            // token back, so the form has nothing to echo. Sending null lets it
            // fall back to the stored value.
            payload.token = (form && form.token && form.token()) || null;

            self.testBusy(true);
            self.testResult("Contacting the board...");
            self.testOk(false);

            command("test", payload).done(function (result) {
                var body = result || {};
                self.testOk(!!body.ok);
                if (body.ok) {
                    self.testResult(body.message || "Connected.");
                } else {
                    self.testResult(
                        (body.error || "Could not reach the board.") +
                        (body.hint ? " " + body.hint : "")
                    );
                }
            }).fail(function (xhr) {
                self.testOk(false);
                self.testResult(failureText(xhr, "The test could not be run."));
            }).always(function () {
                self.testBusy(false);
            });
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
    //
    // THIS PUSH MUST HAPPEN IN A jQuery READY CALLBACK, and that callback must be
    // registered before main.js's own. Plugin JS is packed into packed_plugins.js,
    // which index.html loads BEFORE packed_core.js, and jQuery runs ready callbacks
    // in registration order - so this runs while OCTOPRINT_VIEWMODELS is still
    // being collected and before main.js resolves dependencies.
    //
    // Earlier versions wrapped this in $(document).on("DOMReady", ...). There is no
    // such event: "DOMReady" appears exactly once in the entire bundled JavaScript,
    // in this very line, because OctoPrint 1.x never fires it. The handler was
    // never called, OCTOPRINT_VIEWMODELS stayed empty, and the plugin was never
    // constructed at all - so the tab showed nothing and every data-bind in the
    // settings page (including its test button) sat there completely inert. That
    // looked identical to a broken plugin and could not be diagnosed from the
    // Python side, which was answering every request correctly.
    OCTOPRINT_VIEWMODELS.push({
        construct: EnclosureFanViewModel,
        dependencies: ["settingsViewModel", "loginStateViewModel"],
        elements: ["#tab_plugin_enclosure_fan", "#settings_plugin_enclosure_fan"]
    });
});
