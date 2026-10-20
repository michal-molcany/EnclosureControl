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
        # settings API.
        return dict(token=[["token"]])

    def on_settings_save(self, data):
        octoprint.plugin.SettingsPlugin.on_settings_save(self, data)
        # Rebuild the client and poll at once: someone who just fixed the host or
        # pasted a token should not sit through the old failure state first.
        self._build_client()
        if self._thread is not None and self._thread.is_alive():
            self._refresh()

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

    def get_api_commands(self):
        return dict(
            # Reads the cache. Does NOT touch the board, so it is instant and
            # cannot fail for network reasons; a dead board is reported ok=false.
            status=dict(),
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