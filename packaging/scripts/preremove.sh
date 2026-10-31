#!/bin/sh
# Before removal (deb: "remove", rpm: $1 = 0). Not on upgrade.
# Stopping zapret also removes its firewall rules.

case "$1" in
    remove|0)
        if [ -d /run/systemd/system ]; then
            systemctl stop zapret.service || true
        fi
        ;;
esac

exit 0
