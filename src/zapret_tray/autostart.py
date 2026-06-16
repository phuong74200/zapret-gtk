"""Manage the per-user autostart entry for zapret-tray.

The "Start at login" toggle writes ``~/.config/autostart/org.zapret.Tray.desktop``.
A user entry overrides a same-named entry in ``/etc/xdg/autostart``, so writing one
with ``X-GNOME-Autostart-enabled=false`` reliably disables autostart even when the
installer also dropped a system-wide entry.
"""

import os

from gi.repository import GLib

_BASENAME = "org.zapret.Tray.desktop"
_SYSTEM = "/etc/xdg/autostart/" + _BASENAME

_TEMPLATE = """[Desktop Entry]
Type=Application
Name=Zapret Tray
Comment=Toggle the zapret DPI-bypass service from the system tray
Exec=zapret-tray --hidden
Icon=zapret-on-symbolic
Terminal=false
X-GNOME-Autostart-enabled={enabled}
"""


def _user_path():
    return os.path.join(GLib.get_user_config_dir(), "autostart", _BASENAME)


def _flagged_off(path):
    try:
        with open(path) as fh:
            for line in fh:
                if line.strip().lower() in (
                    "x-gnome-autostart-enabled=false",
                    "hidden=true",
                ):
                    return True
    except OSError:
        pass
    return False


def is_enabled():
    """True if the app is set to start at login (user entry or system default)."""
    user = _user_path()
    if os.path.exists(user):
        return not _flagged_off(user)
    return os.path.exists(_SYSTEM)


def set_enabled(enabled):
    """Write the user autostart entry, enabled or explicitly disabled."""
    path = _user_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        fh.write(_TEMPLATE.format(enabled="true" if enabled else "false"))
    os.replace(tmp, path)
