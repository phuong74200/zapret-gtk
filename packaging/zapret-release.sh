# Sourced by the build scripts and install.sh: the one place that says which
# zapret is bundled. Everything (scripts, Linux static binaries, winws for
# Windows) comes from this official release zip, verified by SHA-256.
#
# To update zapret: change ZAPRET_VERSION and ZAPRET_SHA256 (GitHub shows the
# digest on the release page), then upload the zip to the mirror release:
#   gh release create zapret-vX.Y zapret-vX.Y.zip --latest=false \
#       --title "zapret vX.Y (build mirror)" --notes "Mirror of the upstream release."

ZAPRET_VERSION=v72.13
ZAPRET_SHA256=c493e33a0dc4eba23a8686efdaba55f59755ad6ade3564aebd9d13f4c65e2e0c
ZAPRET_URL=https://github.com/bol-van/zapret/releases/download/$ZAPRET_VERSION/zapret-$ZAPRET_VERSION.zip
# Copy in this repo's releases, used if the upstream download fails.
ZAPRET_MIRROR_URL=https://github.com/phuong74200/zapret-gtk/releases/download/zapret-$ZAPRET_VERSION/zapret-$ZAPRET_VERSION.zip

# fetch_zapret CACHE_DIR -> prints the path of the verified release zip.
fetch_zapret() {
    local dir=$1 zip=$1/zapret-$ZAPRET_VERSION.zip url
    mkdir -p "$dir"
    if [ ! -f "$zip" ]; then
        for url in "$ZAPRET_URL" "$ZAPRET_MIRROR_URL"; do
            if curl -fsSL --retry 3 -o "$zip.part" "$url" >&2; then
                mv "$zip.part" "$zip"
                break
            fi
            echo "warning: could not download $url" >&2
        done
        rm -f "$zip.part"
        [ -f "$zip" ] || { echo "error: could not download zapret $ZAPRET_VERSION" >&2; return 1; }
    fi
    if ! echo "$ZAPRET_SHA256  $zip" | sha256sum -c --quiet - >&2; then
        echo "error: checksum mismatch for $zip (delete it to re-download)" >&2
        return 1
    fi
    echo "$zip"
}

# stage_zapret ZIP ARCH DEST
# Lay out a ready-to-run /opt/zapret tree in DEST (which is replaced): zapret's
# scripts and data, with the static binaries for ARCH (amd64/x86_64 or
# arm64/aarch64) where its scripts look for them. Leaves out the C sources,
# the prebuilt binaries for other systems and zapret's own installers.
stage_zapret() {
    local zip=$1 arch=$2 dest=$3 zarch tmp src b
    case $arch in
        amd64|x86_64) zarch=linux-x86_64 ;;
        arm64|aarch64) zarch=linux-arm64 ;;
        *) echo "error: unsupported architecture: $arch" >&2; return 1 ;;
    esac
    tmp=$(mktemp -d)
    if ! unzip -q "$zip" -d "$tmp"; then
        rm -rf "$tmp"
        return 1
    fi
    src=$tmp/zapret-$ZAPRET_VERSION
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -a "$src"/. "$dest"/
    rm -rf "$dest"/binaries "$dest"/Makefile "$dest"/install_bin.sh "$dest"/install_easy.sh \
           "$dest"/install_prereq.sh "$dest"/uninstall_easy.sh "$dest"/docs/compile \
           "$dest"/files/huawei "$dest"/nfq "$dest"/tpws "$dest"/ip2net "$dest"/mdig
    for b in nfq/nfqws tpws/tpws ip2net/ip2net mdig/mdig; do
        if [ ! -f "$src/binaries/$zarch/${b#*/}" ]; then
            echo "error: ${b#*/} missing for $zarch in zapret $ZAPRET_VERSION" >&2
            rm -rf "$tmp"
            return 1
        fi
        install -D -m 0755 "$src/binaries/$zarch/${b#*/}" "$dest/$b"
    done
    rm -rf "$tmp"
}
