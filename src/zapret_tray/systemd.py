"""Control and observe the ``zapret.service`` systemd unit over the system D-Bus.

Every operation targets a single unit. ``StartUnit``/``StopUnit`` go through
``org.freedesktop.systemd1.Manager``; with the polkit rule shipped in
``data/50-zapret.rules`` they require no password. Current state is read from the
unit's ``ActiveState`` property.

Why D-Bus instead of shelling out to ``systemctl``: it lets the toggle run as the
unprivileged user, gives a clean async API for the click handler, and a cheap
synchronous read for the poll loop -- all without spawning processes.
"""

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

UNIT = "zapret.service"

_SYSTEMD_NAME = "org.freedesktop.systemd1"
_MANAGER_PATH = "/org/freedesktop/systemd1"
_MANAGER_IFACE = "org.freedesktop.systemd1.Manager"
_UNIT_IFACE = "org.freedesktop.systemd1.Unit"
_PROPS_IFACE = "org.freedesktop.DBus.Properties"

# Allow interactive auth as a fallback: when the polkit rule is installed the call
# is authorized silently; if it is somehow missing, the user gets a prompt instead
# of an opaque failure.
_CALL_FLAGS = Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION

# Treat the transitional states as "on" too, so the switch doesn't flicker off
# while a Type=forking service is still coming up after a start request.
_ACTIVE_STATES = ("active", "activating", "reloading")


class ZapretService:
    """Thin wrapper around systemd's Manager interface for ``zapret.service``."""

    def __init__(self):
        self._bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self._manager = Gio.DBusProxy.new_sync(
            self._bus,
            Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES
            | Gio.DBusProxyFlags.DO_NOT_AUTO_START,
            None,
            _SYSTEMD_NAME,
            _MANAGER_PATH,
            _MANAGER_IFACE,
            None,
        )

    # -- mutating actions (async; never block the UI) ----------------------

    def start_async(self, callback):
        """Start the unit. ``callback(ok: bool, error: str | None)``."""
        self._call_unit_action("StartUnit", callback)

    def stop_async(self, callback):
        """Stop the unit. ``callback(ok: bool, error: str | None)``."""
        self._call_unit_action("StopUnit", callback)

    def _call_unit_action(self, method, callback):
        def _on_done(proxy, result):
            try:
                proxy.call_finish(result)
                callback(True, None)
            except GLib.Error as exc:
                callback(False, exc.message)

        self._manager.call(
            method,
            GLib.Variant("(ss)", (UNIT, "replace")),
            _CALL_FLAGS,
            -1,
            None,
            _on_done,
        )

    # -- read-only state (cheap synchronous call; used by the poll loop) ----

    def is_active(self):
        """Return True iff the unit's ``ActiveState`` is ``active``.

        A not-yet-loaded or unloaded unit is reported as inactive, which is the
        correct user-facing meaning (zapret is off).
        """
        unit_path = self._get_unit_path()
        if unit_path is None:
            return False
        try:
            result = self._bus.call_sync(
                _SYSTEMD_NAME,
                unit_path,
                _PROPS_IFACE,
                "Get",
                GLib.Variant("(ss)", (_UNIT_IFACE, "ActiveState")),
                GLib.VariantType("(v)"),
                Gio.DBusCallFlags.NONE,
                -1,
                None,
            )
            return result.unpack()[0] in _ACTIVE_STATES
        except GLib.Error:
            return False

    def _get_unit_path(self):
        try:
            result = self._manager.call_sync(
                "GetUnit",
                GLib.Variant("(s)", (UNIT,)),
                Gio.DBusCallFlags.NONE,
                -1,
                None,
            )
            return result.unpack()[0]
        except GLib.Error:
            # NoSuchUnit -> the unit is not loaded, i.e. inactive.
            return None
