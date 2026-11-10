#!/bin/sh
# Build the macOS installer: dist/NTSCRT-Installer-macos.pkg (version from VERSION).
# Payload-free: the pkg only carries install.sh + postinstall, which download the
# latest release when it runs. Usage: sh installer/macos/build-pkg.sh [out.pkg]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/dist/NTSCRT-Installer-macos.pkg}"
version="$(tr -d '[:space:]' < "$root/VERSION")"
work="$(mktemp -d "${TMPDIR:-/tmp}/ntscrt-pkg.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/pkgs" "$(dirname "$out")"
cp "$here/scripts/postinstall" "$root/install.sh" "$work/scripts/"
chmod 755 "$work/scripts/postinstall" "$work/scripts/install.sh"
pkgbuild --nopayload --scripts "$work/scripts" --identifier com.ntscrt.plugins.installer \
  --version "$version" "$work/pkgs/ntscrt-scripts.pkg"
sed "s/__VERSION__/$version/g" "$here/distribution.xml" > "$work/distribution.xml"
productbuild --distribution "$work/distribution.xml" --resources "$here/resources" \
  --package-path "$work/pkgs" "$out"
echo "Built $out (version $version)"
