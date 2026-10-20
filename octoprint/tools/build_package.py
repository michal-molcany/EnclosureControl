"""Build the plugin archive the OctoPrint Plugin Manager installs from.

Produces dist/OctoPrint-EnclosureFan-<version>.zip with setup.py at the ROOT of the
archive. That layout is required: the plugin manager pip-installs the archive, and
pip needs a setup.py in the top level - a zip containing an extra wrapping folder
installs as a file called after the folder and the plugin never appears.

Run:  python tools/build_package.py
"""

import os
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PKG = os.path.join(ROOT, "octoprint_enclosure_fan")
DIST = os.path.join(ROOT, "dist")


def read_version():
    """Take the version from setup.py instead of keeping a second copy here.

    A duplicated version is not cosmetic: the archive NAME is what the Plugin
    Manager's update check compares, so a stale copy here ships a zip that is
    labelled 0.3.5, installs 0.3.6 code, and never looks like an update. (That
    had already happened once: this file still said 0.3.4 while setup.py said
    0.3.5.) Reading one source of truth makes the two impossible to disagree.
    """
    with open(os.path.join(ROOT, "setup.py"), encoding="utf-8") as handle:
        match = re.search(r'^plugin_version\s*=\s*"([^"]+)"', handle.read(),
                          re.MULTILINE)
    if not match:
        raise SystemExit("plugin_version not found in setup.py")
    return match.group(1)


VERSION = read_version()
NAME = "OctoPrint-EnclosureFan"

# Everything the installed package needs. __pycache__ is excluded deliberately:
# stale .pyc files in a plugin directory are a classic source of a plugin that
# appears to load old code.
INCLUDE_FILES = ["setup.py", "MANIFEST.in"]
INCLUDE_TREE = [
    "octoprint_enclosure_fan/__init__.py",
    "octoprint_enclosure_fan/c3client.py",
    "octoprint_enclosure_fan/templates/enclosure_fan_tab.html",
    "octoprint_enclosure_fan/templates/enclosure_fan_settings.html",
    "octoprint_enclosure_fan/static/js/enclosure_fan.js",
    "octoprint_enclosure_fan/static/css/enclosure_fan.css",
]


def build():
    if os.path.isdir(DIST):
        shutil.rmtree(DIST)
    os.makedirs(DIST)

    out = os.path.join(DIST, "{0}-{1}.zip".format(NAME, VERSION))
    missing = []
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in INCLUDE_FILES + INCLUDE_TREE:
            path = os.path.join(ROOT, name.replace("/", os.sep))
            if not os.path.isfile(path):
                missing.append(name)
                continue
            # forward slashes always: zip paths are POSIX regardless of host OS
            archive.write(path, name)

    if missing:
        raise SystemExit("missing files:\n  " + "\n  ".join(missing))

    print("built {0} ({1} bytes)".format(out, os.path.getsize(out)))
    with zipfile.ZipFile(out) as archive:
        for info in archive.infolist():
            print("  {0:>7}  {1}".format(info.file_size, info.filename))
        bad = archive.testzip()
        if bad:
            raise SystemExit("corrupt entry: " + bad)
    print("archive verified")
    return out


if __name__ == "__main__":
    build()
    sys.exit(0)