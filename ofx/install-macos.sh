#!/bin/sh
# Installs NTSCRT.ofx.bundle into /Library/OFX/Plugins (DaVinci Resolve's
# OpenFX folder on macOS). Needs sudo. Restart Resolve afterwards.
#
#   sudo ./ofx/install-macos.sh build/ofx/Release/NTSCRT.ofx.bundle
set -eu
BUNDLE="${1:-build/ofx/Release/NTSCRT.ofx.bundle}"
DEST="${2:-/Library/OFX/Plugins}"

if [ ! -f "$BUNDLE/Contents/MacOS/NTSCRT.ofx" ]; then
  echo "Not a built NTSCRT bundle: $BUNDLE (expected Contents/MacOS/NTSCRT.ofx)" >&2
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo (writes to $DEST)." >&2
  exit 1
fi
mkdir -p "$DEST"
rm -rf "$DEST/NTSCRT.ofx.bundle"
cp -R "$BUNDLE" "$DEST/NTSCRT.ofx.bundle"
# Locally built, unsigned binaries: drop quarantine and ad-hoc sign so Resolve can load them.
xattr -dr com.apple.quarantine "$DEST/NTSCRT.ofx.bundle" 2>/dev/null || true
codesign --force --deep --sign - "$DEST/NTSCRT.ofx.bundle" 2>/dev/null || true
echo "Installed $DEST/NTSCRT.ofx.bundle"
