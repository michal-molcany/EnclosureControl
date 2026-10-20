# HTTP client for the EnclosureFanControl (C3) board.
#
# Scope, and why it is this small
# ------------------------------
# The C3 already serves everything this plugin needs on port 8082, so there is no
# firmware work here at all: no new endpoints, no new packet types, no protocol
# revision. See lib/EnclosureUi/README.md for the board-side contract.
#
# Only the stdlib is used, deliberately. OctoPrint already depends on Tornado and
# ships requests, but neither needs to be imported into a client that only ever
# speaks to one small board on the LAN - and urllib keeps the dependency surface
# at zero for anyone vendoring this out of the repo.
#
# Every response from the board, INCLUDING every write, is the same full status
# document (lib/EnclosureUi/README.md: "Every response - including the ones to a
# write - is the same document /api/status returns, so the page can never end up
# showing a value the board does not have"). That is why post_json() returns the
# decoded status rather than None: a write is also a refresh, for free.

import base64
import json
import socket
import urllib.error
import urllib.request

# Path of the one GET that returns everything, in one round trip.
STATUS_PATH = "/api/status"
PID_PATH = "/api/pid"
PID_SAVE_PATH = "/api/pid/save"
PID_DEFAULTS_PATH = "/api/pid/defaults"
MANUAL_PATH = "/api/manual"

# Every POST must carry this, or WebServer hands the body to its form parser
# instead of arg("plain") and the board answers "bad json". curl -d alone does
# NOT set it - the board-side README calls this out explicitly.
JSON_CONTENT_TYPE = "application/json"


class C3Error(Exception):
    """A request to the C3 did not produce a usable status document.

    Carries the HTTP status when there was one, so the UI can tell "the board
    said no" (400 on a bad gain) from "the board did not answer" (timeout, refused
    connection, wrong host). Those need different words shown to the user: one is
    a mistake to correct, the other is a network problem.
    """

    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


class C3Client:
    """Talks to one C3 board.

    Deliberately stateless apart from the credentials: the poller owns the cached
    status, so two clients (or a client and a test) cannot disagree about what the
    board last said.
    """

    def __init__(self, host, port=8082, token="", user="Enclosure", timeout=4.0):
        # mDNS first, IP as a fallback the user can type. "enclosure-fan-control.local"
        # survives the Pi getting a new DHCP lease, which an IP does not.
        self.host = (host or "").strip()
        self.port = int(port or 8082)
        self.token = token or ""
        # "Enclosure", capital E. This is NOT a placeholder: WebServer::authenticate()
        # base64-encodes "<user>:<password>" and compares the WHOLE header, so the
        # username is part of the credential and any other value is rejected even
        # with the right token. The 8081 API uses a different one ("cyd").
        self.user = user
        self.timeout = timeout

    # -- plumbing ---------------------------------------------------------

    def _auth_header(self):
        """HTTP Basic header, or None when no token is configured.

        An empty token disables auth on the board side (the same rule lib/HttpOta
        uses), so sending no header is correct rather than sending an empty one.
        """
        if not self.token:
            return None
        raw = "{0}:{1}".format(self.user, self.token).encode("utf-8")
        return "Basic " + base64.b64encode(raw).decode("ascii")

    def _url(self, path):
        return "http://{0}:{1}{2}".format(self.host, self.port, path)

    def _request(self, path, payload=None, method=None):
        """GET or POST, returning the decoded status document.

        `method` is explicit because some of the board's write routes take NO body
        at all: /api/pid/save and /api/pid/defaults are registered HTTP_POST with
        nothing to send. Inferring the method from "is there a payload" sent those
        two as GETs and the board answered 404 on every press - a route that
        exists and works, reached the wrong way.

        Raises C3Error on any failure. Never returns a partial document: a caller
        that got half a status would render half-truths on a screen whose entire
        job is to say what the board is doing.
        """
        url = self._url(path)
        data = None
        headers = {}
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = JSON_CONTENT_TYPE
        auth = self._auth_header()
        if auth:
            headers["Authorization"] = auth
        if method is None:
            method = "POST" if data is not None else "GET"

        request = urllib.request.Request(url, data=data, headers=headers,
                                         method=method)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read()
        except urllib.error.HTTPError as err:
            # 401 is the most common one by far, and it has a specific fix
            # (wrong token) worth saying out loud rather than as "error".
            detail = ""
            try:
                detail = err.read().decode("utf-8", "replace").strip()
            except Exception:
                pass
            if err.code == 401:
                raise C3Error(
                    "C3 rejected the credentials - check the API token "
                    "(username must be 'Enclosure').", status=401)
            raise C3Error("C3 returned HTTP {0}{1}".format(
                err.code, ": " + detail if detail else ""), status=err.code)
        except urllib.error.URLError as err:
            # Covers refused connections and DNS failures. socket.timeout also
            # surfaces here on most builds, but is caught separately below because
            # a timeout is the common case on a board that has just rebooted.
            raise C3Error("C3 not reachable at {0} - {1}".format(self.host, err.reason))
        except socket.timeout:
            raise C3Error("C3 did not answer within {0:.0f}s".format(self.timeout))

        if not body:
            raise C3Error("C3 returned an empty body")
        try:
            document = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as err:
            raise C3Error("C3 returned a body that is not JSON: {0}".format(err))
        if not isinstance(document, dict):
            raise C3Error("C3 returned JSON that is not an object")
        return document

    # -- reads ------------------------------------------------------------

    def get_status(self):
        return self._request(STATUS_PATH)

    # -- writes. Each returns the FULL status, so a write doubles as a refresh.

    def set_pid(self, kp, ki, kd):
        # The board rejects a missing or non-numeric gain rather than applying a
        # partial tuning, and it echoes the gains actually in force - which may
        # differ from these if it clamped. The caller shows the echo, not the
        # request, so a clamp is visible instead of silent.
        return self._request(PID_PATH, {"kp": kp, "ki": ki, "kd": kd})

    def save_pid(self):
        # POST with an EMPTY body: the board persists whatever gains are already
        # running, so there is nothing to send. Explicit method - see _request().
        return self._request(PID_SAVE_PATH, method="POST")

    def restore_default_pid(self):
        return self._request(PID_DEFAULTS_PATH, method="POST")

    def set_manual(self, mode, louvre, fan):
        """mode 0 = AUTO, 1 = MANUAL. louvre/fan are 0-100.

        NOTE: the board hardcodes material=UNKNOWN and jobActive=0 here - a
        deliberate decision on its side ("a browser has no business claiming a job
        is running"). The consequence for us is real: switching back to AUTO while
        the CYD is offline leaves the C3 on its conservative 50 C fallback target
        instead of the per-material one. See README.md.
        """
        return self._request(MANUAL_PATH, {"mode": int(mode),
                                           "louvre": int(louvre),
                                           "fan": int(fan)})