#!/usr/bin/env bash
# Build the Windows installer: dist/Zapret-<version>-x64.msi
#
# Everything is cross-built on Linux:
#   * zapret-tray.exe   compiled with mingw-w64 from windows/zapret_tray.c
#   * zapret-service.exe  likewise, from windows/zapret_service.c
#   * winws + WinDivert taken from the official zapret release pinned in
#                       packaging/zapret-release.sh
#   * the .msi          built with wixl (msitools) from windows/zapret.wxs
#
# Needs: x86_64-w64-mingw32-gcc, x86_64-w64-mingw32-windres, wixl, python3,
#        curl, unzip.
#   Fedora:        sudo dnf install mingw64-gcc msitools
#   Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 wixl
#
# Override tools with CC=, WINDRES=, WIXL= (and WIXL_EXTDIR= if wixl's UI
# extension lives somewhere unusual).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
. "$ROOT/packaging/zapret-release.sh"
SRC=$ROOT/windows
OUT=$ROOT/build/windows
DIST=$ROOT/dist
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
WIXL=${WIXL:-wixl}

die() { echo "error: $*" >&2; exit 1; }
for tool in "$CC" "$WINDRES" "$WIXL" python3 curl unzip; do
    command -v "$tool" >/dev/null || die "'$tool' not found (see the top of $0 for packages)"
done

[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be x.y.z, got '$VERSION'"

mkdir -p "$OUT" "$DIST"

# Version for the exe resources, from the VERSION file.
cat > "$OUT/version.h" <<EOF
#define APP_VERSION_STR "$VERSION"
#define APP_VERSION_NUM ${VERSION//./, }, 0
EOF

# Windows text files want CRLF line endings.
crlf() { sed 's/\r*$/\r/' "$1" > "$2"; }

echo "==> icons"
python3 "$SRC/make_icons.py" "$OUT"

echo "==> zapret-tray.exe"
cp "$SRC/zapret_tray.manifest" "$OUT/"
"$WINDRES" -I "$OUT" -I "$SRC" "$SRC/zapret_tray.rc" -O coff -o "$OUT/zapret_tray.res.o"
"$CC" -std=c11 -O2 -I "$OUT" -Wall -Wextra -municode -mwindows -s \
    -o "$OUT/zapret-tray.exe" "$SRC/zapret_tray.c" "$OUT/zapret_tray.res.o" \
    -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -luser32

echo "==> zapret-service.exe"
cp "$SRC/zapret_service.manifest" "$OUT/"
"$WINDRES" -I "$OUT" -I "$SRC" "$SRC/zapret_service.rc" -O coff -o "$OUT/zapret_service.res.o"
"$CC" -std=c11 -O2 -I "$OUT" -Wall -Wextra -municode -s \
    -o "$OUT/zapret-service.exe" "$SRC/zapret_service.c" "$OUT/zapret_service.res.o" \
    -lshell32 -ladvapi32

echo "==> winws $ZAPRET_VERSION"
zip=$(fetch_zapret "$ROOT/build/cache")
rm -rf "$OUT/release"
unzip -q "$zip" "zapret-$ZAPRET_VERSION/binaries/windows-x86_64/*" \
    "zapret-$ZAPRET_VERSION/docs/LICENSE.txt" -d "$OUT/release"
ZAPRET_LICENSE=$OUT/release/zapret-$ZAPRET_VERSION/docs/LICENSE.txt
WINWS=$OUT/release/zapret-$ZAPRET_VERSION/binaries/windows-x86_64
for f in winws.exe cygwin1.dll WinDivert.dll WinDivert64.sys; do
    [ -f "$WINWS/$f" ] || die "$f missing from the zapret release"
done

echo "==> installer files"
crlf "$SRC/winws.conf" "$OUT/winws.conf"
crlf "$SRC/README.txt" "$OUT/README.txt"
crlf "$ROOT/LICENSE" "$OUT/LICENSE.txt"
crlf "$ZAPRET_LICENSE" "$OUT/LICENSE-zapret.txt"
# License page of the installer (wixl's UI reads "License.rtf" from the cwd).
python3 - "$ROOT/LICENSE" "$ZAPRET_LICENSE" > "$OUT/License.rtf" <<'EOF'
import sys
def esc(text):
    out = []
    for ch in text:
        if ch in "\\{}":
            out.append("\\" + ch)
        elif ord(ch) > 127:
            out.append("\\u%d?" % (ord(ch) if ord(ch) < 32768 else ord(ch) - 65536))
        else:
            out.append(ch)
    return "".join(out).replace("\n", "\\par\n")
parts = [
    ("Zapret tray app", open(sys.argv[1], encoding="utf-8").read()),
    ("zapret / winws (bundled)", open(sys.argv[2], encoding="utf-8").read()),
    ("WinDivert (bundled)", "WinDivert is dual-licensed under the GNU Lesser General Public "
     "License v3 and the GNU General Public License v2.\nhttps://reqrypt.org/windivert.html\n"),
]
print(r"{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\fs16")
for title, body in parts:
    print(r"{\b " + esc(title) + r"}\par\par")
    print(esc(body.strip() + "\n") + r"\par")
print("}")
EOF

echo "==> msi"
MSI=$DIST/Zapret-$VERSION-x64.msi
extdir=()
[ -n "${WIXL_EXTDIR:-}" ] && extdir=(--extdir "$WIXL_EXTDIR")
(cd "$OUT" && "$WIXL" -a x64 --ext ui "${extdir[@]}" \
    -D Version="$VERSION" -D BuildDir="$OUT" -D WinwsDir="$WINWS" \
    -o "$MSI" "$SRC/zapret.wxs")

echo
echo "built $MSI ($(du -h "$MSI" | cut -f1))"
