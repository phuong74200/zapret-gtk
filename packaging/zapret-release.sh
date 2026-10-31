# Sourced by the build scripts. The official zapret release whose prebuilt
# binaries are bundled: winws for Windows, static nfqws/tpws for Linux. Keep it
# at the same version as the vendored ./zapret tree.

ZAPRET_VERSION=v72.1
ZAPRET_SHA256=8824b576b6b481b82396ff3dfd3530c834d1d345f0ab63051080244996918f19
ZAPRET_URL=https://github.com/bol-van/zapret/releases/download/$ZAPRET_VERSION/zapret-$ZAPRET_VERSION.zip

# fetch_zapret CACHE_DIR -> prints the path of the verified release zip.
fetch_zapret() {
    local dir=$1 zip=$1/zapret-$ZAPRET_VERSION.zip
    mkdir -p "$dir"
    if [ ! -f "$zip" ]; then
        curl -fsSL --retry 3 -o "$zip.part" "$ZAPRET_URL" >&2
        mv "$zip.part" "$zip"
    fi
    if ! echo "$ZAPRET_SHA256  $zip" | sha256sum -c --quiet - >&2; then
        echo "error: checksum mismatch for $zip (delete it to re-download)" >&2
        return 1
    fi
    echo "$zip"
}
