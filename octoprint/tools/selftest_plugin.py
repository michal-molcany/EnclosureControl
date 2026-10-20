"""Exercise the plugin's logic without OctoPrint installed.

Stubs out octoprint.plugin so __init__.py can be imported and driven directly.
The point is to prove the two things that matter and cannot be checked by reading:

  1. the CYD-link gate refuses a manual write, and
  2. the poller degrades to "not connected" instead of dying on a bad board.

Run:  python tools/selftest_plugin.py
"""

import logging
import os
import sys
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.join(os.path.dirname(HERE), "octoprint_enclosure_fan")
sys.path.insert(0, PKG)

# The board token comes from the ENVIRONMENT, never from a literal in this file.
#
# This repository is public, and a token hardcoded here would be published with
# it - which is a live credential to the enclosure hardware, not a sample value.
# Set ENC_API_TOKEN to run the live-board section; without it those checks are
# skipped and everything before them still runs.
TOKEN = os.environ.get("ENC_API_TOKEN", "")
C3 = os.environ.get("ENC_C3_HOST", "enclosure-fan-control.local")

# --- stub octoprint -------------------------------------------------------
octoprint = types.ModuleType("octoprint")
plugin_mod = types.ModuleType("octoprint.plugin")
core_mod = types.ModuleType("octoprint.plugin.core")


class _Mixin:
    def __init__(self, *a, **k):
        pass


for name in ("StartupPlugin", "ShutdownPlugin", "SettingsPlugin",
             "AssetPlugin", "TemplatePlugin", "SimpleApiPlugin"):
    setattr(plugin_mod, name, type(name, (_Mixin,), {}))
plugin_mod.SettingsPlugin.on_settings_save = lambda self, data: None
octoprint.plugin = plugin_mod
sys.modules["octoprint"] = octoprint
sys.modules["octoprint.plugin"] = plugin_mod
sys.modules["octoprint.plugin.core"] = core_mod

# flask is imported for jsonify. Stubbed so the selftest needs no web framework;
# the real thing turns the dict into a JSON response, the stub hands it back.
flask_mod = types.ModuleType("flask")


def _jsonify(*args, **kwargs):
    if len(args) == 1 and not kwargs and isinstance(args[0], dict):
        return args[0]
    return dict(*args, **kwargs)


flask_mod.jsonify = _jsonify
sys.modules["flask"] = flask_mod

import c3client  # noqa: E402

# Import as a PACKAGE, not as a bare "__init__" module: __init__.py uses a
# relative import ("from .c3client import ..."), which has no parent package when
# the file is loaded directly. Adding the directory that CONTAINS the package and
# importing by name gives it one.
sys.path.insert(0, os.path.dirname(PKG))
import octoprint_enclosure_fan as init  # noqa: E402

EnclosureFanPlugin = init.EnclosureFanPlugin

failures = []


def check(name, condition, detail=""):
    print(("  PASS  " if condition else "  FAIL  ") + name + (("  -> " + detail) if detail and not condition else ""))
    if not condition:
        failures.append(name)


class FakeSettings:
    """Matches the SettingsPlugin wrapper contract: get() takes ONE positional
    argument - the path list - and returns None for an unset value.

    A plain dict looks equivalent and is not: dict.get(["host"]) raises
    TypeError (unhashable list) rather than answering. That difference is
    precisely what let a two-positional .get(path, default) call ship to a live
    OctoPrint, so the stub is deliberately shaped like the real wrapper.
    """

    def __init__(self, values):
        self._values = values

    def get(self, path):
        if not isinstance(path, (list, tuple)) or len(path) != 1:
            raise AssertionError(
                "Settings.get() takes exactly one path list, got %r" % (path,))
        return self._values.get(path[0])


class FakePlugin(EnclosureFanPlugin):
    """Real logic, scripted board."""

    def __init__(self, client):
        EnclosureFanPlugin.__init__(self)
        self._client = client
        # _logger is injected by OctoPrint's plugin manager, so the plain class
        # does not have one. on_settings_save() logs deliberately, so the stub
        # has to provide the attribute or the test would fail on the harness
        # rather than on the behaviour.
        self._logger = logging.getLogger("selftest")
        self._settings = FakeSettings({"host": C3, "port": 8082, "token": TOKEN,
                                   "poll": True})


def call(plugin, command, data):
    """Invoke on_api_command the way OctoPrint does and return the response body.

    Error paths return a (body, status) tuple, mirroring a Flask response;
    success returns the body alone.
    """
    out = plugin.on_api_command(command, data)
    if isinstance(out, tuple):
        return out[0]
    return out


def make_client(**over):
    calls = []

    class C:
        def __init__(self):
            self.status = {"ok": True, "chamber": 252, "chamberValid": True,
                           "state": 0, "mode": 0, "louvre": 0, "fan": 0,
                           "manualLouvre": 0, "manualFan": 0, "remote": False,
                           "pidSaved": True, "ip": "10.0.0.1", "fw": "0.4.1",
                           "rssi": -60, "heap": 190000, "uptime": 900,
                           "pid": {"kp": 6, "ki": 0.1, "kd": 0,
                                   "min": {"kp": 0, "ki": 0, "kd": 0},
                                   "max": {"kp": 50, "ki": 5, "kd": 50}}}

        def get_status(self):
            calls.append("GET")
            return dict(self.status)

        def set_manual(self, mode, louvre, fan):
            calls.append(("manual", mode, louvre, fan))
            self.status = dict(self.status, mode=mode, louvre=louvre, fan=fan)
            return dict(self.status)

    c = C()
    for k, v in over.items():
        setattr(c, k, v)
    return c, calls


print("== manual-control gate ==")

# CYD offline -> manual allowed
p = FakePlugin(make_client()[0])
res = call(p, "setmanual", {"mode": 1, "louvre": 40, "fan": 55})
check("manual allowed when CYD offline", res["ok"] is True, str(res))
check("write refreshed the cache", p._snapshot()["status"]["mode"] == 1)

# CYD linked -> manual refused
client, calls = make_client()
client.status["remote"] = True
p = FakePlugin(client)
res = call(p, "setmanual", {"mode": 1, "louvre": 40, "fan": 55})
check("manual refused when CYD linked", res["ok"] is False, str(res))
check("refusal explains why", "CYD is linked" in res["error"], res["error"])
check("no manual request was sent", not any(c[0] == "manual" for c in calls if isinstance(c, tuple)), str(calls))

print()
print("== input validation (before any network call) ==")
for bad, why in [({"mode": 5, "louvre": 0, "fan": 0}, "bad mode"),
                 ({"mode": 1, "louvre": 200, "fan": 0}, "louvre > 100"),
                 ({"mode": 1, "louvre": 0, "fan": -1}, "fan < 0"),
                 ({"mode": 1, "louvre": 0}, "missing fan")]:
    p = FakePlugin(make_client()[0])
    res = call(p, "setmanual", bad)
    check("rejects " + why, res["ok"] is False, str(res))

print()
print("== on_api_command dispatch ==")
p = FakePlugin(make_client()[0])
p._refresh()
res = call(p, "status", {})
check("status returns the cache", res["ok"] is True, str(res))
check("status payload has a status block", isinstance(res["status"], dict),
      str(res))
res = call(p, "nosuchcommand", {})
check("unknown command is refused", res["ok"] is False, str(res))
check("unknown command lists the valid ones", "setmanual" in res["error"],
      res["error"])

print()
print("== poller resilience ==")

flaky = types.SimpleNamespace(status=None)
boom = [0]


class Flaky(c3client.C3Client):
    def get_status(self):
        boom[0] += 1
        if boom[0] < 3:
            raise c3client.C3Error("simulated board failure")
        return {"ok": True, "pid": {"kp": 6}}


p = FakePlugin(make_client()[0])
class DeadBoard(c3client.C3Client):
    """Always fails, the way an unplugged or wrong-host board does."""

    def get_status(self):
        raise c3client.C3Error("simulated dead board")


p._client = Flaky(C3, 8082, TOKEN)
for _ in range(3):
    p._refresh()
snap = p._snapshot()
check("recovers after transient failures", snap["ok"] is True, str(snap))
check("error cleared on recovery", snap["error"] is None)

p._client = DeadBoard(C3, 8082, TOKEN)
for _ in range(2):
    p._refresh()
snap = p._snapshot()
check("dead board reports not-connected", snap["ok"] is False, str(snap))
check("dead board reports a message", bool(snap["error"]), str(snap))

print()
print("== settings plumbing ==")

# _build_client must work against the real wrapper contract: ONE positional
# argument, None for an unset value. A second positional is a TypeError on a
# live OctoPrint, so it cannot be papered over with a dict here.
p = FakePlugin(make_client()[0])
p._settings = FakeSettings({"host": "10.201.93.245", "port": 8082,
                           "token": "sometoken", "poll": True})
p._build_client()
check("_build_client builds a client", p._client is not None)

# Unset keys fall back through `or <default>`, never a second argument.
p._settings = FakeSettings({"token": "sometoken", "poll": True})
p._build_client()
check("unset host coerced to ''", p._client.host == "",
      repr(getattr(p._client, "host", "?")))
check("unset port coerced to 8082", p._client.port == 8082,
      repr(getattr(p._client, "port", "?")))

# Only an explicit False stops the poll; an absent value means "poll".
check("poll defaults to on",
      p._settings.get(["poll"]) is not False)

print()
print("== settings save (the token must survive a save it is not part of) ==")

# The settings dialog posts the whole plugin subtree back on every save. The
# token is masked to None on read, so it arrives as None unless it was retyped -
# and the parent implementation MERGES, which means None overwrites the stored
# token. Capture what is actually handed to the parent.
saved = {}
_original_parent_save = init.octoprint.plugin.SettingsPlugin.on_settings_save
init.octoprint.plugin.SettingsPlugin.on_settings_save = lambda self, data: saved.update(data)

p = FakePlugin(make_client()[0])
p._settings = FakeSettings({"host": "10.0.0.9", "port": 8082, "token": "stored",
                            "poll": True})

p.on_settings_save({"host": " 10.0.0.9 ", "port": "8082", "token": None, "poll": True})
check("a masked (None) token is not written back", "token" not in saved, str(saved))
check("a string port is normalised to int", saved.get("port") == 8082, str(saved))
check("the host is trimmed", saved.get("host") == "10.0.0.9", repr(saved.get("host")))
check("the client is rebuilt from settings after a save", p._client.port == 8082)

saved.clear()
p.on_settings_save({"token": ""})
check("an empty token does not clear the stored one", "token" not in saved, str(saved))

saved.clear()
p.on_settings_save({"token": "retyped"})
check("a retyped token is persisted", saved.get("token") == "retyped", str(saved))

saved.clear()
p.on_settings_save({"port": "not-a-number"})
check("a non-numeric port is dropped, not persisted as a string",
      "port" not in saved, str(saved))

init.octoprint.plugin.SettingsPlugin.on_settings_save = _original_parent_save

print()
print("== test connection ==")

# on_test builds a C3Client directly, so swap the class for the duration of the
# test to prove what it SENDS (host/port/token fallback) and how it classifies
# the answer.
_transport_calls = []


class FakeTransport:
    def __init__(self, host, port=8082, token="", user="Enclosure", timeout=4.0):
        self.host = host
        self.port = port
        self.token = token
        _transport_calls.append(self)

    def get_status(self):
        return {"ok": True, "fw": "0.4.1", "ip": "10.201.93.245", "chamber": 240,
                "chamberValid": True, "remote": True, "pid": {}}


_init_client = init.C3Client
init.C3Client = FakeTransport

p = FakePlugin(make_client()[0])
p._settings = FakeSettings({"host": "saved.local", "port": 8082,
                            "token": "saved-token", "poll": True})

_transport_calls.clear()
res = call(p, "test", {"host": "10.201.93.245", "port": "8082", "token": None})
check("test succeeds against a good answer", res.get("ok") is True, str(res))
check("test reports the firmware version", res.get("fw") == "0.4.1", str(res))
check("test reports the CYD link", res.get("cydLinked") is True, str(res))
check("test falls back to the stored token when the field is masked",
      _transport_calls and _transport_calls[-1].token == "saved-token",
      str(_transport_calls and _transport_calls[-1].token))
check("test uses the entered host, not the saved one",
      _transport_calls and _transport_calls[-1].host == "10.201.93.245")

_transport_calls.clear()
call(p, "test", {"host": "", "port": None, "token": None})
check("blank fields fall back to the saved settings",
      _transport_calls and _transport_calls[-1].host == "saved.local"
      and _transport_calls[-1].port == 8082, str(_transport_calls))

# A refused command comes back as (body, status) - a 400, not a 200 with ok=false.
bad = p.on_api_command("test", {"host": "h", "port": "not-a-number"})
check("a non-numeric port is refused before any request", isinstance(bad, tuple) and bad[1] == 400, str(bad))

init.C3Client = _init_client

# Hints are the whole point of the feature: they name the remedy, and 8081 in
# particular has to be called out, because it answers and looks like a bad token.
p = FakePlugin(make_client()[0])
check("a wrong port 8081 hint names 8082",
      "8082" in p._test_hint("refused", "h", 8081, True),
      p._test_hint("refused", "h", 8081, True))
check("auth on 8081 is blamed on the port, not the token",
      "8082" in p._test_hint("auth", "h", 8081, True),
      p._test_hint("auth", "h", 8081, True))
check("a 404 (the right token on 8081) still names 8082",
      "8082" in p._test_hint("http", "h", 8081, True, status=404),
      p._test_hint("http", "h", 8081, True, status=404))
check("auth on 8082 points at ENC_API_TOKEN",
      "ENC_API_TOKEN" in p._test_hint("auth", "h", 8082, True),
      p._test_hint("auth", "h", 8082, True))
check("a missing token is reported as missing",
      "No API token" in p._test_hint("auth", "h", 8082, False),
      p._test_hint("auth", "h", 8082, False))
check("a DNS failure suggests the IP address",
      "IP" in p._test_hint("dns", "name.local", 8082, True),
      p._test_hint("dns", "name.local", 8082, True))

print()
print("== page wiring (the two mistakes that made the page look dead) ==")

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(HERE)
with open(os.path.join(PKG_DIR, "octoprint_enclosure_fan", "templates",
                       "enclosure_fan_settings.html"), encoding="utf-8") as handle:
    settings_html = handle.read()
with open(os.path.join(PKG_DIR, "octoprint_enclosure_fan", "static", "js",
                       "enclosure_fan.js"), encoding="utf-8") as handle:
    client_js = handle.read()


def code_only(source):
    """Strip // and /* */ comments.

    The comments in this file TALK about simpleApiCommand and PNotify.notify, so
    a naive substring count measures documentation rather than behaviour.
    """
    out = []
    in_block = False
    for line in source.splitlines():
        stripped = line.strip()
        if in_block:
            if "*/" in stripped:
                in_block = False
            continue
        if stripped.startswith("/*"):
            if "*/" not in stripped:
                in_block = True
            continue
        if stripped.startswith("//"):
            continue
        out.append(line)
    return "\n".join(out)


client_code = code_only(client_js)

check("settings template binds through the settings view model",
      "settingsViewModel.settings.plugins.enclosure_fan.host" in settings_html)
check("settings template has no bare settings.plugins binding",
      "value: settings.plugins" not in settings_html)
check("view model exposes the dependency under that name",
      "self.settingsViewModel = parameters[0]" in client_code)
check("simpleApiCommand is only called through the promise wrapper",
      client_code.count("simpleApiCommand(") == 1,
      str(client_code.count("simpleApiCommand(")))
check("no PNotify.notify (does not exist in OctoPrint 1.11)",
      "PNotify.notify" not in client_code)
check("the test button is wired", "testConnection" in settings_html
      and "self.testConnection" in client_code)

# The registration that makes any of the above reachable. OctoPrint 1.x has no
# "DOMReady" event - waiting for one means the view model is never constructed,
# nothing is ever bound, and the whole page is inert while looking fine.
check("the view model is registered with OCTOPRINT_VIEWMODELS",
      "OCTOPRINT_VIEWMODELS.push(" in client_code)
check("registration does not wait for a DOMReady event (OctoPrint never fires one)",
      "DOMReady" not in client_code)
check("registration happens inside the single jQuery ready callback",
      client_code.count("$(function") == 1)

print()
print("== live board (skipped unless ENC_API_TOKEN is set) ==")
if not TOKEN:
    print("  SKIP  ENC_API_TOKEN not set - not running against real hardware")
else:
    live = c3client.C3Client(C3, 8082, TOKEN)
    try:
        live.get_status()
    except c3client.C3Error as err:
        print("  SKIP  board unreachable:", err)
    else:
        p = FakePlugin(live)
        # on_status() reads the CACHE, so populate it the way the poller thread
        # would. Testing it without a poll would only prove the cache starts empty.
        p._refresh()
        snap = p.on_status({})
        check("live status cached", snap["ok"] is True)
        check("live remote flag present", "remote" in snap["status"],
              str(snap["status"].keys()))
        if snap["status"].get("remote"):
            res = call(p, "setmanual", {"mode": 1, "louvre": 0, "fan": 0})
            check("live: manual refused with CYD linked", res["ok"] is False,
                  str(res))
        else:
            check("live: CYD offline so manual permitted", True)

print()
if failures:
    print("FAILURES:", failures)
    sys.exit(1)
print("all checks passed")