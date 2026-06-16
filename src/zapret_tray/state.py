"""Persist the user's last on/off choice so it can be restored at startup.

Stored in ``~/.config/zapret-tray/enabled`` as ``1`` or ``0``. Only the app's own
toggles update it; changes made elsewhere (e.g. ``systemctl``) are not recorded.
"""

import os

from gi.repository import GLib


def _path():
    return os.path.join(GLib.get_user_config_dir(), "zapret-tray", "enabled")


def load():
    """Return True/False for the saved state, or None if it was never set."""
    try:
        with open(_path()) as fh:
            return fh.read().strip() == "1"
    except OSError:
        return None


def save(enabled):
    path = _path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        fh.write("1" if enabled else "0")
    os.replace(tmp, path)
