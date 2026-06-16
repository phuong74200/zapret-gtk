"""GTK3 app for toggling zapret.

The control surface is, in order of convenience:
  * the tray icon's menu, whose first item is an on/off check item;
  * middle-click or scroll on the tray icon (instant flip);
  * a window with a big switch (opened from the menu).

A tray icon (StatusNotifierItem/AppIndicator) is only an icon + a menu -- it
cannot itself be a switch widget, and on GNOME a left-click always opens the
menu. So "click to toggle" lives in the menu's check item, with middle-click and
scroll as shortcuts. The icon reflects state (filled vs. outlined shield).

Closing the window keeps the app running in the tray. Start/stop go through
``zapret.service`` over the system D-Bus (see :mod:`zapret_tray.systemd`); all
controls mirror the service's real state via a short poll.

If the AppIndicator typelib/extension isn't available, the app runs as a plain
window -- closing it then quits, since there is no tray to return from.
"""

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")  # must match Gtk 3; else Gdk 4.0 auto-loads
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from . import autostart, state
from .systemd import ZapretService

try:
    gi.require_version("AyatanaAppIndicator3", "0.1")
    from gi.repository import AyatanaAppIndicator3 as AppIndicator
except (ValueError, ImportError):
    try:
        gi.require_version("AppIndicator3", "0.1")
        from gi.repository import AppIndicator3 as AppIndicator
    except (ValueError, ImportError):
        AppIndicator = None  # window-only mode

APP_ID = "org.zapret.Tray"
POLL_SECONDS = 3
ICON_ON = "zapret-on-symbolic"
ICON_OFF = "zapret-off-symbolic"


class ZapretApp(Gtk.Application):
    def __init__(self):
        super().__init__(
            application_id=APP_ID,
            flags=Gio.ApplicationFlags.HANDLES_COMMAND_LINE,
        )
        self._service = ZapretService()
        self._active = None       # tri-state until first sync
        self._busy = False        # a start/stop request is in flight
        self._syncing = False     # guard: programmatic control updates
        self._autostart_syncing = False
        self._poll_id = 0
        self._hinted_tray = False

        self._window = None
        self._switch = None
        self._state_label = None
        self._autostart_check = None   # window "start at login" checkbox
        self._indicator = None
        self._check = None             # menu on/off item
        self._autostart_item = None    # menu "start at login" item
        self._status_item = None       # menu status label

    # -- lifecycle ----------------------------------------------------------

    def do_startup(self):
        Gtk.Application.do_startup(self)
        self.hold()  # survive window close; we quit explicitly
        self._build_window()
        if AppIndicator is not None:
            self._build_indicator()
        self._refresh(force=True)
        self._apply_autostart_ui(autostart.is_enabled())
        self._restore_saved_state()
        self._poll_id = GLib.timeout_add_seconds(POLL_SECONDS, self._on_poll)

    def do_command_line(self, command_line):
        # Launched with --hidden (autostart) -> stay in the tray, no window.
        if "--hidden" not in command_line.get_arguments():
            self._present()
        return 0

    def do_shutdown(self):
        if self._poll_id:
            GLib.source_remove(self._poll_id)
            self._poll_id = 0
        if self._indicator is not None:
            self._indicator.set_status(AppIndicator.IndicatorStatus.PASSIVE)
            self._indicator = None
        Gtk.Application.do_shutdown(self)

    # -- window -------------------------------------------------------------

    def _build_window(self):
        self._window = Gtk.ApplicationWindow(application=self)
        self._window.set_title("Zapret")
        self._window.set_default_size(340, 200)
        self._window.set_resizable(False)
        self._window.set_position(Gtk.WindowPosition.CENTER)
        self._window.set_icon_name(ICON_ON)
        self._window.connect("delete-event", self._on_close)

        header = Gtk.HeaderBar(show_close_button=True)
        header.set_title("Zapret")
        header.set_subtitle("DPI bypass")
        self._window.set_titlebar(header)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        box.set_border_width(28)
        box.set_valign(Gtk.Align.CENTER)
        box.set_halign(Gtk.Align.CENTER)

        title = Gtk.Label()
        title.set_markup("<big><b>Zapret</b></big>")

        self._switch = Gtk.Switch()
        self._switch.set_halign(Gtk.Align.CENTER)
        self._switch.connect("state-set", self._on_switch)

        self._state_label = Gtk.Label(label="…")
        self._state_label.get_style_context().add_class("dim-label")

        self._autostart_check = Gtk.CheckButton(label="Start at login")
        self._autostart_check.set_halign(Gtk.Align.CENTER)
        self._autostart_check.connect("toggled", self._on_autostart)

        box.add(title)
        box.add(self._switch)
        box.add(self._state_label)
        box.add(self._autostart_check)
        self._window.add(box)

    def _present(self):
        self._window.show_all()
        self._window.present()

    def _on_close(self, *_args):
        if self._indicator is None:
            self.quit()  # no tray to restore from -> closing means quit
            return True
        self._window.hide()
        if not self._hinted_tray:
            self._hinted_tray = True
            self._notify(
                "Zapret is still running",
                "Toggle it from the tray icon, or choose Quit in its menu.",
            )
        return True  # keep running in the tray

    # -- tray ---------------------------------------------------------------

    def _build_indicator(self):
        self._indicator = AppIndicator.Indicator.new(
            APP_ID, ICON_OFF, AppIndicator.IndicatorCategory.SYSTEM_SERVICES
        )
        self._indicator.set_status(AppIndicator.IndicatorStatus.ACTIVE)
        self._indicator.set_title("Zapret")

        menu = Gtk.Menu()

        self._status_item = Gtk.MenuItem(label="zapret: …")
        self._status_item.set_sensitive(False)
        menu.append(self._status_item)

        self._check = Gtk.CheckMenuItem(label="Enable zapret")
        self._check.connect("toggled", self._on_check)
        menu.append(self._check)

        menu.append(Gtk.SeparatorMenuItem())

        self._autostart_item = Gtk.CheckMenuItem(label="Start at login")
        self._autostart_item.connect("toggled", self._on_autostart)
        menu.append(self._autostart_item)

        menu.append(Gtk.SeparatorMenuItem())

        open_item = Gtk.MenuItem(label="Open Zapret…")
        open_item.connect("activate", lambda _i: self._present())
        menu.append(open_item)

        quit_item = Gtk.MenuItem(label="Quit")
        quit_item.connect("activate", lambda _i: self.quit())
        menu.append(quit_item)

        menu.show_all()
        self._indicator.set_menu(menu)
        # Middle-click the panel icon flips the check item; scroll does too.
        self._indicator.set_secondary_activate_target(self._check)
        self._indicator.connect("scroll-event", self._on_scroll)

    # -- control signals ----------------------------------------------------

    def _on_switch(self, _switch, want):
        if not self._syncing:
            self._set_active(want)
        return False  # let the slider move optimistically; poll will confirm

    def _on_check(self, item):
        if not self._syncing:
            self._set_active(item.get_active())

    def _on_scroll(self, _indicator, _delta, direction):
        if direction == Gdk.ScrollDirection.UP:
            self._set_active(True)
        elif direction == Gdk.ScrollDirection.DOWN:
            self._set_active(False)

    def _on_autostart(self, item):
        if self._autostart_syncing:
            return
        enabled = item.get_active()
        autostart.set_enabled(enabled)
        self._apply_autostart_ui(enabled)

    def _apply_autostart_ui(self, enabled):
        self._autostart_syncing = True
        for widget in (self._autostart_check, self._autostart_item):
            if widget is not None and widget.get_active() != enabled:
                widget.set_active(enabled)
        self._autostart_syncing = False

    # -- action core --------------------------------------------------------

    def _set_active(self, want):
        """User intent: turn zapret on/off. Coalesces concurrent requests."""
        if self._busy:
            return
        self._busy = True
        self._set_controls_sensitive(False)

        def done(ok, error):
            self._busy = False
            self._set_controls_sensitive(True)
            if ok:
                self._apply_state(want)
                state.save(want)  # remember the choice; restored at next startup
            else:
                self._notify(
                    "Could not %s zapret" % ("enable" if want else "disable"),
                    error or "Permission denied.",
                )
                self._refresh(force=True)

        (self._service.start_async if want else self._service.stop_async)(done)

    # -- state sync ---------------------------------------------------------

    def _on_poll(self):
        if not self._busy:
            self._refresh()
        return GLib.SOURCE_CONTINUE

    def _refresh(self, force=False):
        active = self._service.is_active()
        if force or active != self._active:
            self._apply_state(active)

    def _restore_saved_state(self):
        """At startup, bring zapret to the user's last saved on/off choice."""
        saved = state.load()
        if saved is None or saved == self._service.is_active():
            return

        def done(_ok, _error):
            self._refresh(force=True)

        (self._service.start_async if saved else self._service.stop_async)(done)

    def _apply_state(self, active):
        self._active = active

        # Update both controls without re-entering their handlers.
        self._syncing = True
        if self._switch is not None:
            self._switch.set_active(active)
            self._switch.set_state(active)
        if self._check is not None:
            self._check.set_active(active)
        self._syncing = False

        if self._state_label is not None:
            self._state_label.set_text("Active" if active else "Inactive")
        if self._indicator is not None:
            self._indicator.set_icon_full(
                ICON_ON if active else ICON_OFF,
                "zapret enabled" if active else "zapret disabled",
            )
        if self._status_item is not None:
            self._status_item.set_label("zapret: on" if active else "zapret: off")

    def _set_controls_sensitive(self, value):
        if self._switch is not None:
            self._switch.set_sensitive(value)
        if self._check is not None:
            self._check.set_sensitive(value)

    # -- helpers ------------------------------------------------------------

    def _notify(self, title, body):
        notification = Gio.Notification.new(title)
        notification.set_body(body)
        self.send_notification("zapret", notification)
