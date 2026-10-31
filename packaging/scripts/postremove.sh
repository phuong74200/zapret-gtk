#!/bin/sh
# After removal (deb: "remove" / "purge", rpm: $1 = 0).

case "$1" in
    remove|purge|0)
        if [ -d /run/systemd/system ]; then
            systemctl daemon-reload || true
        fi
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
        fi
        ;;
esac

# deb purge: drop the rest of /opt/zapret (config is removed by dpkg).
if [ "$1" = purge ]; then
    rm -rf /opt/zapret
fi

exit 0
