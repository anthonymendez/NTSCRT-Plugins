#!/bin/sh
# Installs NTSCRT.ofx.bundle into /Library/OFX/Plugins (DaVinci Resolve's
# OpenFX folder on macOS). Asks for your password (sudo). Restart Resolve after.
#
#   ./install-macos.sh [path/to/NTSCRT.ofx.bundle]     # default: the bundle next to this script
#   ./install-macos.sh --uninstall
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/OFX/Plugins"

if [ "${1:-}" = "--uninstall" ]; then
  [ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
  rm -rf "$DEST/NTSCRT.ofx.bundle"
  echo "Removed $DEST/NTSCRT.ofx.bundle"
  exit 0
fi

BUNDLE="${1:-$HERE/NTSCRT.ofx.bundle}"
case "$BUNDLE" in /*) ;; *) BUNDLE="$PWD/$BUNDLE" ;; esac
if [ ! -f "$BUNDLE/Contents/MacOS/NTSCRT.ofx" ]; then
  echo "Not an NTSCRT bundle: $BUNDLE (expected Contents/MacOS/NTSCRT.ofx)" >&2
  exit 1
fi
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$BUNDLE"

mkdir -p "$DEST"
rm -rf "$DEST/NTSCRT.ofx.bundle"
cp -R "$BUNDLE" "$DEST/NTSCRT.ofx.bundle"
# Unsigned/ad-hoc builds: drop the download quarantine and ad-hoc sign so Resolve can load them.
xattr -dr com.apple.quarantine "$DEST/NTSCRT.ofx.bundle" 2>/dev/null || true
codesign --force --deep --sign - "$DEST/NTSCRT.ofx.bundle" 2>/dev/null || true
echo "Installed $DEST/NTSCRT.ofx.bundle"
echo "Restart DaVinci Resolve. The effect is under Effects > OpenFX > Filters > NTSCRT."
