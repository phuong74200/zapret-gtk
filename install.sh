#!/usr/bin/env bash
#
# zapret-gtk installer.
#
# Run as your normal user (NOT root). Privileged steps use sudo individually so
# that the user-session steps (enabling the GNOME extension, launching the tray)
# still run as you. Fedora / dnf only.
#
#   bash install.sh            # interactive
#   bash install.sh --yes      # assume "yes" for dependency install + prompts
#   bash install.sh --no-deps  # skip dnf, build and install only
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZAPRET_SRC="$SCRIPT_DIR/zapret"

ZAPRET_DEST="/opt/zapret"
APP_DEST="/usr/local/lib/zapret-tray"
LAUNCHER="/usr/local/bin/zapret-tray"
ICON_DEST="/usr/local/share/icons/hicolor/scalable/status"
APP_DESKTOP="/usr/local/share/applications/org.zapret.Tray.desktop"
AUTOSTART_DESKTOP="/etc/xdg/autostart/org.zapret.Tray.desktop"
POLKIT_RULE="/etc/polkit-1/rules.d/50-zapret.rules"
SERVICE_DEST="/etc/systemd/system/zapret.service"
APPINDICATOR_UUID="appindicatorsupport@rgcjonas.gmail.com"

ASSUME_YES=0
DO_DEPS=1
for arg in "$@"; do
    case "$arg" in
        --yes|-y) ASSUME_YES=1 ;;
        --no-deps) DO_DEPS=0 ;;
        -h|--help) sed -n '3,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

info()  { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

confirm() {
    # confirm "Question?" -> 0 (yes) / 1 (no). Honors --yes.
    [ "$ASSUME_YES" = 1 ] && return 0
    local reply
    read -r -p "$* [y/N] " reply
    case "$reply" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# --- preconditions ---------------------------------------------------------

[ "$(id -u)" != 0 ] || die "Run this as your normal user, not root (it uses sudo where needed)."
command -v dnf >/dev/null 2>&1 || die "This installer targets Fedora (dnf not found)."
command -v sudo >/dev/null 2>&1 || die "sudo is required."

TARGET_USER="$USER"
info "Installing zapret-gtk for user '$TARGET_USER'."

# zapret source is vendored in-tree (version-locked with this app; no submodule).
[ -f "$ZAPRET_SRC/Makefile" ] || die "zapret sources missing at $ZAPRET_SRC."

# --- dependencies ----------------------------------------------------------

BUILD_DEPS=(gcc make zlib-devel libcap-devel libnetfilter_queue-devel libmnl-devel)
RUNTIME_DEPS=(python3-gobject gtk3 libayatana-appindicator-gtk3 \
              gnome-shell-extension-appindicator nftables curl)

if [ "$DO_DEPS" = 1 ]; then
    info "Dependencies to install via dnf:"
    printf '   build:   %s\n' "${BUILD_DEPS[*]}"
    printf '   runtime: %s\n' "${RUNTIME_DEPS[*]}"
    if confirm "Install them now?"; then
        sudo dnf install -y "${BUILD_DEPS[@]}" "${RUNTIME_DEPS[@]}"
    else
        warn "Skipping dependency install. Build/runtime may fail without them."
    fi
fi

# --- build zapret ----------------------------------------------------------

info "Building zapret (nfqws, tpws, ...)..."
make -C "$ZAPRET_SRC" clean >/dev/null 2>&1 || true
make -C "$ZAPRET_SRC"
[ -x "$ZAPRET_SRC/nfq/nfqws" ] || die "Build did not produce nfq/nfqws."

# --- install zapret to /opt/zapret -----------------------------------------

info "Installing zapret to $ZAPRET_DEST ..."
sudo mkdir -p "$ZAPRET_DEST"
# -a preserves the symlinks make created (nfq/nfqws -> binaries/my/nfqws, etc.).
sudo cp -a "$ZAPRET_SRC"/. "$ZAPRET_DEST"/
sudo rm -rf "$ZAPRET_DEST/.git"
# cp -a preserves the source's user_home_t SELinux label, which systemd (init_t)
# is not allowed to execute. Relabel to the /opt defaults so the service runs.
if command -v restorecon >/dev/null 2>&1; then
    sudo restorecon -RF "$ZAPRET_DEST" || true
fi

# config: seed from zapret's template, then append our overrides once.
if [ ! -f "$ZAPRET_DEST/config" ]; then
    sudo cp "$ZAPRET_DEST/config.default" "$ZAPRET_DEST/config"
fi
if ! sudo grep -q "zapret-gtk's install.sh" "$ZAPRET_DEST/config"; then
    sudo tee -a "$ZAPRET_DEST/config" < "$SCRIPT_DIR/data/config.overrides" >/dev/null
fi

# --- optional: blockcheck --------------------------------------------------

if confirm "Run zapret's blockcheck now to find a working DPI strategy? (takes a while, needs network)"; then
    info "Launching blockcheck (interactive). Note the recommended nfqws options it prints."
    sudo "$ZAPRET_DEST/blockcheck.sh" || warn "blockcheck exited non-zero; continuing."
    echo
    info "Paste the recommended nfqws options from blockcheck to use them,"
    info "or just press Enter to keep the bundled default strategy."
    read -r -p "NFQWS_OPT= " nfqws_opt || true
    if [ -n "${nfqws_opt:-}" ]; then
        printf 'NFQWS_OPT="%s"\n' "$nfqws_opt" | sudo tee -a "$ZAPRET_DEST/config" >/dev/null
        info "Applied your NFQWS_OPT to $ZAPRET_DEST/config."
    fi
fi

# --- systemd service -------------------------------------------------------

info "Installing systemd unit (not enabled at boot; the tray controls it)..."
sudo cp "$ZAPRET_DEST/init.d/systemd/zapret.service" "$SERVICE_DEST"
sudo systemctl daemon-reload

# --- polkit rule (passwordless start/stop for this user) -------------------

info "Installing polkit rule for passwordless control of zapret.service ..."
sed "s/{USER}/$TARGET_USER/g" "$SCRIPT_DIR/data/50-zapret.rules" \
    | sudo tee "$POLKIT_RULE" >/dev/null

# --- install the tray app --------------------------------------------------

info "Installing the tray app ..."
sudo rm -rf "$APP_DEST"
sudo mkdir -p "$APP_DEST"
sudo cp -a "$SCRIPT_DIR/src/zapret_tray" "$APP_DEST/zapret_tray"

sudo tee "$LAUNCHER" >/dev/null <<EOF
#!/bin/sh
# Launch the zapret-gtk tray app.
export PYTHONPATH="$APP_DEST\${PYTHONPATH:+:\$PYTHONPATH}"
exec python3 -m zapret_tray "\$@"
EOF
sudo chmod 0755 "$LAUNCHER"

sudo mkdir -p "$ICON_DEST"
sudo cp "$SCRIPT_DIR"/data/icons/zapret-on-symbolic.svg "$ICON_DEST/"
sudo cp "$SCRIPT_DIR"/data/icons/zapret-off-symbolic.svg "$ICON_DEST/"
sudo gtk-update-icon-cache -f /usr/local/share/icons/hicolor >/dev/null 2>&1 || true

sudo mkdir -p "$(dirname "$APP_DESKTOP")" "$(dirname "$AUTOSTART_DESKTOP")"
# App launcher opens the window; autostart starts hidden (tray only) at login.
sudo cp "$SCRIPT_DIR/data/org.zapret.Tray.desktop" "$APP_DESKTOP"
sed 's|^Exec=zapret-tray$|Exec=zapret-tray --hidden|' \
    "$SCRIPT_DIR/data/org.zapret.Tray.desktop" | sudo tee "$AUTOSTART_DESKTOP" >/dev/null

# --- enable AppIndicator extension + launch --------------------------------

info "Enabling the AppIndicator GNOME extension ..."
gnome-extensions enable "$APPINDICATOR_UUID" 2>/dev/null \
    && info "Enabled $APPINDICATOR_UUID." \
    || warn "Could not enable $APPINDICATOR_UUID now (it may need a re-login first)."

if confirm "Launch the tray app now?"; then
    setsid "$LAUNCHER" >/dev/null 2>&1 &
fi

cat <<EOF

$(info "Done.")

Next steps:
  1. Open "Zapret" from your apps (or run: zapret-tray). A window with an
     on/off switch appears -- flip it to start/stop zapret, no password prompt.
     Closing the window keeps it running in the tray; Quit from the tray menu.
  2. For the TRAY icon to show on Wayland, enable the AppIndicator extension and
     LOG OUT / back IN once (Wayland can't hot-reload shell extensions):
         gnome-extensions enable $APPINDICATOR_UUID
     (The window works without it; the tray is just where it hides on close.)
  3. Verify:  systemctl is-active zapret    (active when toggled on)
              journalctl -u zapret -n 20    (nfqws + nft rules)

Config lives at $ZAPRET_DEST/config. To re-tune the DPI strategy later:
     sudo $ZAPRET_DEST/blockcheck.sh
Uninstall with:  bash uninstall.sh
EOF
