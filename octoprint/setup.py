# OctoPrint plugin packaging.
#
# The important part of this file is that it actually CALLS setup().
#
# OctoPrint plugins do not hand-roll their setup() arguments - they pass them to
# octoprint_setuptools.create_plugin_setup_parameters(), which is bundled with
# OctoPrint and builds the correct metadata (including the plugin_* attributes
# OctoPrint reads to identify the plugin).
#
# A setup.py that only assigns plugin_name = "..." and friends is NOT enough: pip
# runs it to prepare metadata, finds no egg-info, and the install dies with
#   ERROR: No .egg-info directory found in /tmp/pip-pip-egg-info-XXXX
# which is exactly what happened the first time. That message points at pip, but
# the cause is a setup.py that never called setup().
import os

from setuptools import setup

# Plugins declare their identity as module-level variables, which
# create_plugin_setup_parameters() reads. Keeping them named this way also means
# a tool scanning setup.py can read the plugin's identity without importing it.
plugin_name = "Enclosure Fan Control"
plugin_package = "octoprint_enclosure_fan"
plugin_author = "EnclosureDisplay"
plugin_author_email = "michal.molcany@betterask.erni"
plugin_description = (
    "Live enclosure state, runtime PID tuning and a manual fan/louvre override "
    "for the EnclosureFanControl (C3) board. Talks to the board's existing HTTP "
    "API on port 8082, so no firmware changes are required."
)
plugin_version = "0.3.7"
plugin_license = "AGPLv3"
plugin_url = "https://github.com/michal-molcany/EnclosureControl"

# Nothing beyond OctoPrint itself: the board client uses urllib from the stdlib.
plugin_requires = []

# Templates, JS and CSS are NOT Python modules, so pip will not pick them up on
# its own. Without these globs the package installs but OctoPrint finds no
# templates, no assets, and the plugin loads as an invisible stub - which is the
# failure mode of the old `enclosure_control` this replaces.
plugin_additional_data = [
    "templates/*.html",
    "static/js/*.js",
    "static/css/*.css",
]

plugin_additional_packages = []
plugin_ignored_packages = []
plugin_load_after = []
plugin_additional_commands = []
plugin_commands = []


def read_long_description():
    """Fold README.md into the PyPI long description, if it is there.

    Optional on purpose: a source checkout without README.md must still install,
    so a missing file is not an error.
    """
    here = os.path.abspath(os.path.dirname(__file__))
    readme = os.path.join(here, "README.md")
    if not os.path.isfile(readme):
        return ""
    with open(readme, "r", encoding="utf-8") as handle:
        return handle.read()


try:
    from octoprint_setuptools import create_plugin_setup_parameters

    setup_parameters = create_plugin_setup_parameters(
        identifier=plugin_package.replace("octoprint_", "", 1),
        name=plugin_name,
        version=plugin_version,
        description=plugin_description,
        author=plugin_author,
        mail=plugin_author_email,
        url=plugin_url,
        license=plugin_license,
        package=plugin_package,
        requires=plugin_requires,
        additional_data=plugin_additional_data,
        additional_packages=plugin_additional_packages,
        ignored_packages=plugin_ignored_packages,
    )
    # The helper builds the standard dict; these two are ours to add, because
    # create_plugin_setup_parameters() has no parameter for either.
    #
    # python_requires is NOT optional cosmetics. Without it the installed
    # distribution carries no Python constraint, and OctoPrint falls back to its
    # own default of ">=2.7,<3" - which marks the plugin INCOMPATIBLE on any
    # Python 3 host, so it is listed but never loaded. That is exactly what
    # happened on first install; the fix is to state the real requirement here.
    setup_parameters["long_description"] = read_long_description()
    setup_parameters["long_description_content_type"] = "text/markdown"
    # OctoPrint 1.11 runs on Python 3.7+; the target Pi is on 3.11.
    setup_parameters["python_requires"] = ">=3.7"
except ImportError:
    # octoprint_setuptools ships with OctoPrint. Its absence means this is being
    # built somewhere it cannot work - fail loudly with the reason rather than
    # silently producing a package OctoPrint will not recognise.
    raise SystemExit(
        "octoprint_setuptools is required to build this plugin. It is bundled "
        "with OctoPrint; install it with:\n"
        "    pip install OctoPrint-Server stock"
    )

setup(**setup_parameters)
