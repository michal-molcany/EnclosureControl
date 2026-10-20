# OctoPrint plugin: fan control for the EnclosureFanControl (C3) board.
#
# What this is
# ------------
# A tab inside OctoPrint showing the enclosure's live state, letting the PID gains
# be tuned, and offering a manual fan/louvre override.
#
# It is a CLIENT of an API that already exists. The C3 has served /api/status and
# the PID/manual routes on port 8082 since before this plugin existed (see
# lib/EnclosureUi/), so there is no firmware change, no protocol revision and no
# new ESP-NOW packet here. Everything below is OctoPrint-side.
#
# Why port 8082 and not 8081
# --------------------------
# Both of the board's JSON APIs come from the same C3, but 8082 is the one to use:
#   - it returns `remote` (is a CYD linked?), which is what gates manual control;
#   - it returns pid.min / pid.max / pid.defaults, so the accepted ranges live in
#     enclosure_pid.h and cannot drift out of sync with a copy in this file;
#   - it returns `chamberValid` instead of making us test for INT16_MIN;
#   - EVERY write returns the same full status document, so a write is also a
#     refresh and the UI can never show a value the board does not have.
# Port 8081 (/api/enclosure, username "cyd") is the CYD's machine-to-machine
# channel. It is deliberately not used - see README.md for the one case where its
# extra command fields would help.
#
# Threading
# ---------
# A single background thread polls /api/status at 1 Hz and keeps the latest
# document in memory. The UI reads that cache and never waits on the board, so an
# unreachable C3 shows "offline" rather than a spinner, and a slow board cannot
# stall OctoPrint's request handling.
#
# Writes go straight to the board on the calling thread (OctoPrint's handler
# pool). They are small LAN round trips, and - usefully - each returns a fresh
# status document that replaces the cache, so the UI updates from the board's own
# answer instead of waiting for the next poll.

import threading
import time

import flask
import octoprint.plugin

from .c3client import C3Client, C3Error


class CommandFailed(Exception):
    """Raised by the SimpleApiPlugin handlers to signal a clean, user-facing error.

    Deliberately a plain local exception rather than OctoPrint's own
    CommandFailed. Importing that is not safe: on 1.11.8 the plausible homes
    include octoprint.server.api, and importing it pulls in
    octoprint.server.api.settings, whose module scope runs
    `from octoprint.server import ... printer ...`. Executed before the server
    object exists - which is exactly when a plugin module is imported - that
    binds printer=None permanently and every core API endpoint starts 500ing.
    The error format produced here is cosmetically different; the cost of getting
    it wrong is OctoPrint's UI.
    """

# Poll cadence. Matches the C3's own telemetry interval (ENC_TELEMETRY_INTERVAL_MS,
# 1 s) so the tab never shows a value older than the board would send anyway, and
# is gentle enough not to matter to an ESP32 WebServer.
POLL_INTERVAL_S = 1.0

# How long a request may take before it is reported as a failure rather than left
# hanging. Generous for a LAN board; the point is to bound the wait.
REQUEST_TIMEOUT_S = 6.0

# The settings dialog's "Test connection" button has someone watching it, so it
# gives up sooner than the background poller would.
TEST_TIMEOUT_S = 5.0


class EnclosureFanPlugin(octoprint.plugin.StartupPlugin,
                         octoprint.plugin.ShutdownPlugin,
                         octoprint.plugin.SettingsPlugin,
                         octoprint.plugin.AssetPlugin,
                         octoprint.plugin.TemplatePlugin,
                         octoprint.plugin.SimpleApiPlugin):

    def __init__(self):
        super().__init__()
        self._client = None
        self._stop = threading.Event()
        self._thread = None
        # Cached board status plus the last error. A plain dict because it is
        # handed straight to JSON; _cache_lock guards every read and write.
        self._cache = {"ok": False, "status": None, "error": "starting up"}
        self._cache_lock = threading.Lock()

    # ------------------------------------------------------------------ #
    # Settings
    # ------------------------------------------------------------------ #

    def get_settings_defaults(self):
        return dict(
            # mDNS name first: it survives the Pi taking a new DHCP lease, which a
            # stored IP does not. An IP works just as well if mDNS is blocked.
            host="enclosure-fan-control.local",
            port=8082,
            # Left EMPTY on purpose. This is not a default to guess at - the token
            # is a shared secret from the firmware's git-ignored
            # include/enclosure_api.h, and shipping one here would put a live
            # credential in the repository. The board treats an empty token as
            # "auth disabled", so an unconfigured plugin simply gets a 401 rather
            # than silently driving anything.
            token="",
            # Set false to stop polling (e.g. while debugging something else). The
            # tab then shows the last known state, frozen.
            poll=True,
        )

    def get_settings_restricted_paths(self):
        # Mark the token sensitive so it is masked rather than echoed by the
        # settings API. "never" is a DOCUMENTED level and is what was meant all
        # along: an earlier version used the key "token", which is not a level at
        # all, and an unknown key falls back to `conditions.get(level, False)` -
        # i.e. it happened to mask the token by accident and would have kept
        # masking it even for admins if the fallback ever changed.
        return dict(never=[["token"]])

    def on_settings_save(self, data):
        data = dict(data or {})

        # `token` is masked to None on read (get_settings_restricted_paths above),
        # so the settings dialog posts None back whenever the field was not
        # retyped. SettingsPlugin.on_settings_save MERGES the incoming dict over
        # the stored values - None is a value, not an omission - so passing it
        # through would overwrite a perfectly good token with null and leave the
        # board answering 401. Dropping the key leaves the stored token alone,
        # which is also what the field's "leave blank to keep" help text promises.
        if not data.get("token"):
            data.pop("token", None)

        # Normalise what the form sends. A Knockout `value` binding on an
        # <input type="number"> hands back a STRING, and persisting port as
        # "8082" alongside an integer default is the kind of type drift that
        # works until something compares it by identity.
        if "host" in data:
            data["host"] = str(data["host"] or "").strip()
        if "port" in data:
            try:
                data["port"] = int(data["port"])
            except (TypeError, ValueError):
                data.pop("port", None)

        octoprint.plugin.SettingsPlugin.on_settings_save(self, data)

        # Rebuild the client and poll at once: someone who just fixed the host or
        # pasted a token should not sit through the old failure state first.
        self._build_client()
        if self._thread is not None and self._thread.is_alive():
            self._refresh()

        # Log that the save arrived, and with what. A settings page that silently
        # discards input is invisible otherwise: nothing in OctoPrint's log says
        # "settings received", so there is no way to tell a broken save from a
        # save that never happened. The token is reported as set/unset, never
        # echoed - this log is world-readable through the Logging plugin.
        self._logger.info(
            "settings saved: host=%r port=%r token=%s poll=%s",
            self._client.host if self._client else None,
            self._client.port if self._client else None,
            "set" if (self._client and self._client.token) else "unset",
            self._settings.get(["poll"]) is not False,
        )

    def _build_client(self):
        # NOTE: SettingsPlugin hands back a wrapper whose get() takes exactly ONE
        # positional argument - the path list. A second positional (a "default")
        # is forwarded as a third argument to Settings.get(path, **kwargs) and
        # raises TypeError: Settings.get() takes 2 positional arguments but 3
        # were given. An unset value comes back as None, so coerce here.
        self._client = C3Client(
            host=self._settings.get(["host"]) or "",
            port=self._settings.get(["port"]) or 8082,
            token=self._settings.get(["token"]) or "",
            timeout=REQUEST_TIMEOUT_S,
        )

    # ------------------------------------------------------------------ #
    # Lifecycle
    # ------------------------------------------------------------------ #

    def on_after_startup(self):
        self._build_client()
        self._stop.clear()
        self._thread = threading.Thread(target=self._poll_loop,
                                        name="enclosure_fan_poll")
        self._thread.daemon = True
        self._thread.start()

    def on_shutdown(self):
        self._stop.set()
        thread = self._thread
        if thread is not None:
            # Bounded: the loop spends most of its life between polls, so do not
            # wait indefinitely. It is a daemon and holds nothing OctoPrint needs.
            thread.join(timeout=3.0)
        self._thread = None

    def _poll_loop(self):
        while not self._stop.is_set():
            if self._settings.get(["poll"]) is not False:
                self._refresh()
            # wait() rather than sleep() so shutdown is immediate instead of
            # taking up to a full poll interval.
            self._stop.wait(POLL_INTERVAL_S)

    def _refresh(self):
        """One poll. Never raises: a bad board must not kill the thread."""
        client = self._client
        if client is None:
            return
        try:
            status = client.get_status()
        except C3Error as err:
            self._set_cache(False, None, str(err))
            return
        except Exception as err:  # noqa: BLE001
            # A bug in here must degrade to "offline", not stop the poller.
            self._set_cache(False, None, "unexpected error: {0}".format(err))
            return
        self._set_cache(True, status, None)

    def _set_cache(self, ok, status, error):
        with self._cache_lock:
            self._cache = {"ok": ok, "status": status, "error": error}

    def _snapshot(self):
        with self._cache_lock:
            return dict(self._cache)

    # ------------------------------------------------------------------ #
    # SimpleApiPlugin - the endpoints the tab calls
    # ------------------------------------------------------------------ #

    def is_api_protected(self):
        # OctoPrint 1.11.2 added this and warns every start-up about plugins that
        # leave it at the default. The default is False today and will become
        # True. Declaring it is not cosmetic: these commands drive real hardware,
        # so an anonymous request must not reach them. Any authenticated user may
        # use the tab; it is not admin-only, because operating the enclosure is
        # the same class of action as pressing a control in the Control tab.
        return True

    def get_api_commands(self):
        return dict(
            # Reads the cache. Does NOT touch the board, so it is instant and
            # cannot fail for network reasons; a dead board is reported ok=false.
            status=dict(),
            # Probes the board ONCE with values that may not be saved yet, so the
            # settings dialog can tell a typo from a firewall before the user
            # commits them. Never reads or writes the cache.
            test=dict(),
            setpid=dict(parameters=[
                dict(name="kp", type="number", optional=False),
                dict(name="ki", type="number", optional=False),
                dict(name="kd", type="number", optional=False),
            ]),
            savepid=dict(),
            defaultpid=dict(),
            setmanual=dict(parameters=[
                dict(name="mode", type="number", optional=False),
                dict(name="louvre", type="number", optional=False),
                dict(name="fan", type="number", optional=False),
            ]),
        )

    def on_api_command(self, command, data):
        """Route a SimpleApiPlugin command to its handler.

        OctoPrint's SimpleApiPlugin does NOT auto-dispatch to on_<command>
        methods: its on_api_command is a no-op that returns None, so a plugin
        defining only per-command methods answers every POST with 204 and the tab
        renders nothing. The routing has to be written out.

        CommandFailed is caught here rather than raised: OctoPrint's plugin view
        catches everything else and turns it into a bare 500, which would discard
        the explanation the gate exists to give.
        """
        handlers = {
            "status": self.on_status,
            "test": self.on_test,
            "setpid": self.on_setpid,
            "savepid": self.on_savepid,
            "defaultpid": self.on_defaultpid,
            "setmanual": self.on_setmanual,
        }
        handler = handlers.get(command)
        if handler is None:
            return flask.jsonify({
                "ok": False, "status": None,
                "error": "Unknown command: {0}. Valid commands: {1}.".format(
                    command, ", ".join(sorted(handlers))),
            }), 400

        try:
            result = handler(data)
        except CommandFailed as err:
            return flask.jsonify({
                "ok": False, "status": None, "error": str(err)}), 400
        return flask.jsonify(result)

    def on_status(self, data):
        return self._snapshot()

    # ------------------------------------------------------------------ #
    # "Test connection" - the validation the settings page needs
    # ------------------------------------------------------------------ #

    def on_test(self, data):
        """Check one host/port/token triple against the live board and explain.

        Deliberately separate from the poller. It has to test what the user is
        LOOKING AT, which may not be saved yet, and it has to be able to say "the
        token is wrong" rather than leave that to be inferred from a banner that
        only ever says the board is offline.

        The token is masked on read, so the form cannot send it back. Any field
        that arrives empty therefore falls back to the saved setting - which is
        also why the field is documented as "leave blank to keep the current
        token".
        """
        data = data or {}

        host = str(data.get("host") or "").strip()
        if not host:
            host = str(self._settings.get(["host"]) or "").strip()

        raw_port = data.get("port")
        if raw_port in (None, ""):
            raw_port = self._settings.get(["port"])
        try:
            port = int(raw_port or 8082)
        except (TypeError, ValueError):
            raise CommandFailed(
                "Port must be a number - 8082 is the board's status API - got "
                "{0!r}".format(raw_port))

        token = str(data.get("token") or "").strip()
        if not token:
            token = str(self._settings.get(["token"]) or "")

        if not host:
            raise CommandFailed(
                "Enter the board's address first - an IP such as 10.201.93.245, "
                "or its .local name.")
        if not 1 <= port <= 65535:
            raise CommandFailed("Port must be between 1 and 65535, got {0}".format(port))

        client = C3Client(host=host, port=port, token=token, timeout=TEST_TIMEOUT_S)
        started = time.time()
        tested = {"host": host, "port": port, "tokenProvided": bool(token)}

        try:
            status = client.get_status()
        except C3Error as err:
            return {
                "ok": False,
                "kind": err.kind,
                "error": str(err),
                "hint": self._test_hint(err.kind, host, port, bool(token),
                                        status=err.status),
                "tested": tested,
                "elapsed": round(time.time() - started, 2),
            }

        # JSON that parses but has none of the status document's keys is some
        # OTHER service on that port. Reporting "connected" there would be a lie
        # that costs an hour to unpick, so check for the fields every firmware
        # since 0.1 has sent.
        missing = [key for key in ("fw", "chamber") if key not in status]
        if missing:
            return {
                "ok": False,
                "kind": "wrong_board",
                "error": "Something answered at {0}:{1}, but not with the "
                         "enclosure status document (no {2}).".format(
                             host, port, " or ".join(missing)),
                "hint": self._test_hint("wrong_board", host, port, bool(token)),
                "tested": tested,
                "elapsed": round(time.time() - started, 2),
            }

        fw = status.get("fw") or "unknown"
        reported = status.get("ip") or host
        message = "Connected to the board at {0} (firmware {1}).".format(reported, fw)
        if status.get("remote"):
            message += (" The CYD is linked, so manual control stays disabled "
                        "while it is plugged in.")

        return {
            "ok": True,
            "kind": "connected",
            "message": message,
            "error": None,
            "hint": None,
            "fw": fw,
            "ip": reported,
            "cydLinked": bool(status.get("remote")),
            "chamber": status.get("chamber"),
            "chamberValid": bool(status.get("chamberValid")),
            "status": status,
            "tested": tested,
            "elapsed": round(time.time() - started, 2),
        }

    def _test_hint(self, kind, host, port, token_given, status=None):
        """Turn a failure kind into the one sentence that fixes it.

        Port 8081 is called out explicitly because it is the mistake that
        actually happened: it is a real HTTP API on the same board, it answers,
        and what it says depends on the credentials. With a wrong token it is a
        401 (looks like a bad token); with the right one it is a 404 for
        /api/status (looks like the board is broken). Neither is about the token.
        """
        if kind == "auth":
            if port != 8082:
                return ("The board answered on port {0}, but that is not this API. "
                        "Use 8082: 8081 is the CYD's channel and expects a "
                        "different username, 8080 is the OTA uploader.").format(port)
            if not token_given:
                return ("No API token is set. Paste the ENC_API_TOKEN value from the "
                        "firmware's include/enclosure_api.h.")
            return ("The token was refused. Copy ENC_API_TOKEN again from "
                    "include/enclosure_api.h - if the board was re-flashed with a "
                    "new token, the old one no longer works.")

        if kind == "dns":
            return ("The name '{0}' did not resolve. mDNS (.local) is often blocked "
                    "on managed networks - use the board's IP address "
                    "instead.").format(host)

        if kind == "http":
            if status == 404:
                return ("The board answered 404 for /api/status on port {0} - that "
                        "port serves something else. Use 8082.").format(port)
            return ("The board answered HTTP {0}. That is not an authentication "
                    "failure, so the address is right and the port is probably "
                    "wrong: the status API is on 8082.").format(status)

        if kind == "refused":
            if port == 8081:
                return ("Port 8081 is the CYD's machine channel, not this API. "
                        "Use 8082.")
            return ("Nothing is listening on port {0}. The board serves its status "
                    "API on 8082 - check the port, and that this is not the OTA "
                    "port 8080.").format(port)

        if kind == "timeout":
            return ("The board accepted the connection but did not answer in time. "
                    "It may be rebooting; try again in a few seconds.")

        if kind in ("empty", "not_json", "not_object", "wrong_board"):
            return ("Something answered on port {0}, but it is not the enclosure "
                    "API. Use port 8082.").format(port)

        return ""

    def _write(self, action):
        """Run one board write and fold its returned status into the cache.

        The board answers every write with the full status, so this both reports
        the outcome and refreshes the tab from the board's own answer - the UI
        never has to guess whether the value it now shows is the one in force.
        """
        client = self._client
        if client is None:
            raise CommandFailed("Plugin is not configured yet")
        try:
            status = action(client)
        except C3Error as err:
            raise CommandFailed(str(err))
        self._set_cache(True, status, None)
        return {"ok": True, "status": status, "error": None}

    def on_setpid(self, data, **kwargs):
        kp, ki, kd = data.get("kp"), data.get("ki"), data.get("kd")
        if kp is None or ki is None or kd is None:
            raise CommandFailed("kp, ki and kd are all required")
        return self._write(lambda c: c.set_pid(kp, ki, kd))

    def on_savepid(self, **kwargs):
        return self._write(lambda c: c.save_pid())

    def on_defaultpid(self, **kwargs):
        return self._write(lambda c: c.restore_default_pid())

    def on_setmanual(self, data, **kwargs):
        mode = data.get("mode")
        louvre = data.get("louvre")
        fan = data.get("fan")
        if mode is None or louvre is None or fan is None:
            raise CommandFailed("mode, louvre and fan are all required")
        if mode not in (0, 1):
            raise CommandFailed("mode must be 0 (auto) or 1 (manual)")
        for name, value in (("louvre", louvre), ("fan", fan)):
            if not 0 <= value <= 100:
                raise CommandFailed("{0} must be 0-100".format(name))

        # The gate that matters, enforced HERE and not only in the UI.
        #
        # The CYD re-sends its own command every 2 s (ENC_CMD_HEARTBEAT_INTERVAL_MS)
        # and the C3 treats any received command as proof the link is alive, so a
        # manual override set while the CYD is linked is reverted within about two
        # seconds - the board drops it and the sliders snap back on their own.
        # Rather than let someone discover that by watching numbers move, refuse
        # the write outright.
        #
        # Checked against the LIVE board rather than the cache, so the gate is
        # correct even if the tab was opened a moment before the CYD came up.
        client = self._client
        try:
            live = client.get_status()
        except C3Error as err:
            raise CommandFailed(
                "Could not check whether the CYD is linked: {0}".format(err))
        if live.get("remote"):
            raise CommandFailed(
                "The CYD is linked and re-sends its own settings every 2 seconds, "
                "which would immediately override anything set here. Unplug the "
                "display to take control from OctoPrint.")

        return self._write(lambda c: c.set_manual(mode, louvre, fan))

    # ------------------------------------------------------------------ #
    # Assets and templates
    # ------------------------------------------------------------------ #

    def get_assets(self):
        # No stock bundle: this tab needs neither jQuery nor Bootstrap, and
        # OctoPrint already provides the Knockout the 1.x templates bind with.
        return dict(js=["js/enclosure_fan.js"], css=["css/enclosure_fan.css"])

    def is_template_autoescaped(self):
        # Both templates are static HTML: they interpolate no Jinja variables at
        # all (all live data arrives through Knockout bindings), so enabling
        # autoescape cannot change what they render. Declaring it stops the
        # start-up warning and keeps the plugin working when 1.13 makes
        # autoescaping unconditional. OctoPrint 1.11 added this method; older
        # versions never call it.
        return True

    def get_template_configs(self):
        # `template` is a BARE FILENAME, resolved inside this plugin's templates/
        # directory. Passing a path prefixed with the package name
        # ("octoprint_enclosure_fan/enclosure_fan_tab.html") looks reasonable and
        # resolves to templates/octoprint_enclosure_fan/... - which does not exist -
        # so the template silently fails to load and the tab never appears.
        #
        # custom_bindings=True: these templates carry their own Knockout
        # data-bind attributes, which OctoPrint must not try to auto-generate.
        return [
            dict(type="settings", custom_bindings=True,
                 template="enclosure_fan_settings.html"),
            dict(type="tab", custom_bindings=True,
                 template="enclosure_fan_tab.html"),
        ]


# Python compatibility - REQUIRED, and the mechanism is not the obvious one.
#
# OctoPrint does NOT read this from the distribution's Requires-Python field. It
# reads the module attribute __plugin_pythoncompat__ (see
# ControlProperties.attr_pythoncompat in octoprint/plugin/core.py) and falls back
# to ">=2.7,<3" when it is absent - which marks the plugin INCOMPATIBLE on any
# Python 3 host, so it installs, appears in the Plugin Manager, and is then never
# loaded. That is exactly what happened here before this line existed.
__plugin_pythoncompat__ = ">=2.7,<4"

# An INSTANCE, not the class.
#
# OctoPrint 1.x mixin plugins set __plugin_implementation__ to an already-
# constructed instance; the plugin manager then calls initialize() on it.
# Assigning the class instead looks equivalent and imports fine, but the manager
# ends up calling Plugin.initialize() unbound and dies with
#   TypeError: Plugin.initialize() missing 1 required positional argument: 'self'
# which surfaces as "Exception while initializing plugin ... disabling it" and
# leaves a plugin that installs, lists, and does nothing.
#
# (Blueprint plugins in 2.x take the class; this build is 1.11.8 and uses mixins.)
__plugin_implementation__ = EnclosureFanPlugin()