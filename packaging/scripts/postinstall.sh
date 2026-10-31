#!/bin/sh
# After install or upgrade (deb: "configure ...", rpm: $1 = 1 or 2).
# zapret.service is not enabled at boot: the tray app starts and stops it.

if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    # Upgraded while zapret was on: restart it on the new files.
    systemctl try-restart zapret.service || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi

exit 0
