#!/usr/bin/env bash
# Build the Linux packages into dist/:
#   zapret-gtk_<version>-1_{amd64,arm64}.deb     Debian 12+, Ubuntu 24.04+
#   zapret-gtk-<version>-1.{x86_64,aarch64}.rpm  Fedora
#
# No compiling: zapret's scripts and static binaries both come from the official
# release pinned in packaging/zapret-release.sh, so one package works across
# distro versions.
#
# Needs: nfpm (https://nfpm.goreleaser.com), curl, unzip.
# Override with NFPM=, and ARCHES="amd64" to build one architecture.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
. "$ROOT/packaging/zapret-release.sh"
NFPM=${NFPM:-nfpm}
ARCHES=${ARCHES:-amd64 arm64}
OUT=$ROOT/build/linux
DIST=$ROOT/dist

die() { echo "error: $*" >&2; exit 1; }
for tool in "$NFPM" curl unzip; do
    command -v "$tool" >/dev/null || die "'$tool' not found"
done
grep -q "__version__ = \"$VERSION\"" "$ROOT/src/zapret_tray/__init__.py" \
    || die "src/zapret_tray/__init__.py __version__ does not match VERSION ($VERSION)"

mkdir -p "$OUT" "$DIST"
zip=$(fetch_zapret "$ROOT/build/cache")

for arch in $ARCHES; do
    echo "==> $arch"
    STAGE=$OUT/stage-$arch
    rm -rf "$STAGE"
    mkdir -p "$STAGE/opt" "$STAGE/app" "$STAGE/icons"

    # /opt/zapret: zapret's scripts and data with the static binaries for $arch.
    Z=$STAGE/opt/zapret
    stage_zapret "$zip" "$arch" "$Z"

    # Default config, as install.sh makes it: zapret's template + our overrides.
    cat "$Z/config.default" "$ROOT/data/config.overrides" > "$STAGE/config"

    # The tray app, launched the same way install.sh does it.
    cp -r "$ROOT/src/zapret_tray" "$STAGE/app/"
    find "$STAGE/app" -name __pycache__ -prune -exec rm -rf {} +
    cat > "$STAGE/zapret-tray" <<'EOF'
#!/bin/sh
# Launch the zapret-gtk tray app.
export PYTHONPATH="/usr/lib/zapret-tray${PYTHONPATH:+:$PYTHONPATH}"
exec python3 -m zapret_tray "$@"
EOF
    cp "$ROOT"/data/icons/zapret-{on,off}-symbolic.svg "$STAGE/icons/"
    cp "$ROOT/data/org.zapret.Tray.desktop" "$STAGE/"
    sed 's|^Exec=zapret-tray$|Exec=zapret-tray --hidden|' \
        "$ROOT/data/org.zapret.Tray.desktop" > "$STAGE/autostart.desktop"
    cp "$Z/init.d/systemd/zapret.service" "$STAGE/"
    cp "$ROOT/packaging/50-zapret.rules" "$STAGE/"

    # nfpm doesn't expand variables in file paths, so fill them in here.
    sed -e "s|\${ARCH}|$arch|g" -e "s|\${VERSION}|$VERSION|g" \
        -e "s|\${STAGE}|$STAGE|g" -e "s|\${ROOT}|$ROOT|g" \
        "$ROOT/packaging/nfpm.yaml" > "$STAGE/nfpm.yaml"
    for fmt in deb rpm; do
        "$NFPM" package -f "$STAGE/nfpm.yaml" -p "$fmt" -t "$DIST/"
    done
done

echo
ls -1 "$DIST"/zapret-gtk*"$VERSION"* 2>/dev/null
