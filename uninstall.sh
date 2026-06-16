#!/usr/bin/env bash
#
# zapret-gtk uninstaller. Reverses install.sh. Run as your normal user.
# Leaves the repo's ./zapret submodule checkout alone (that's source, not an
# install). Pass --purge to also delete /opt/zapret (config, host lists).
#
set -euo pipefail

ZAPRET_DEST="/opt/zapret"
APP_DEST="/usr/local/lib/zapret-tray"
LAUNCHER="/usr/local/bin/zapret-tray"
ICON_DEST="/usr/local/share/icons/hicolor/scalable/status"
APP_DESKTOP="/usr/local/share/applications/org.zapret.Tray.desktop"
AUTOSTART_DESKTOP="/etc/xdg/autostart/org.zapret.Tray.desktop"
POLKIT_RULE="/etc/polkit-1/rules.d/50-zapret.rules"
SERVICE_DEST="/etc/systemd/system/zapret.service"

PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }

[ "$(id -u)" != 0 ] || { echo "Run as your normal user, not root." >&2; exit 1; }

info "Stopping the tray app ..."
pkill -u "$USER" -f 'python3 -m zapret_tray' 2>/dev/null || true

info "Stopping zapret.service ..."
sudo systemctl stop zapret 2>/dev/null || true
sudo systemctl disable zapret 2>/dev/null || true

info "Removing app, service, polkit rule, icons, autostart ..."
sudo rm -f "$LAUNCHER" "$APP_DESKTOP" "$AUTOSTART_DESKTOP" "$POLKIT_RULE" \
           "$SERVICE_DEST" \
           "$ICON_DEST/zapret-on-symbolic.svg" \
           "$ICON_DEST/zapret-off-symbolic.svg"
sudo rm -rf "$APP_DEST"
sudo systemctl daemon-reload

# per-user autostart override + saved toggle state
rm -f "$HOME/.config/autostart/org.zapret.Tray.desktop"
rm -rf "$HOME/.config/zapret-tray"
sudo gtk-update-icon-cache -f /usr/local/share/icons/hicolor >/dev/null 2>&1 || true

if [ "$PURGE" = 1 ]; then
    info "Purging $ZAPRET_DEST ..."
    sudo rm -rf "$ZAPRET_DEST"
else
    info "Left $ZAPRET_DEST in place (use --purge to remove config + host lists)."
fi

info "Done. The AppIndicator GNOME extension was left enabled; disable it via"
info "  gnome-extensions disable appindicatorsupport@rgcjonas.gmail.com  if unwanted."
