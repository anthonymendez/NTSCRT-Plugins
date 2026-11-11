#!/usr/bin/env bash
# Install Apple's FxPlug SDK on a CI macOS runner from a private URL.
#
#   FXPLUG_SDK_URL=https://... scripts/ci/install_fxplug_sdk.sh
#
# The URL may point at the SDK exactly as Apple ships it (.dmg containing an
# installer .pkg), at the .pkg itself, or at a .zip of either (or a .zip of an
# already-installed /Library/Developer tree containing Frameworks/FxPlug.framework).
# FXPLUG_SDK_SHA256 (required) pins the download. Never echoes the URL.
set -euo pipefail

: "${FXPLUG_SDK_URL:?FXPLUG_SDK_URL is not set}"
: "${FXPLUG_SDK_SHA256:?FXPLUG_SDK_SHA256 is not set: add the SDK download's SHA-256 as a repository secret}"
work="${RUNNER_TEMP:-/tmp}/fxplug-sdk"
rm -rf "$work"; mkdir -p "$work"
dl="$work/download"

echo "Downloading FxPlug SDK (URL hidden)..."
curl --fail --location --silent --show-error --retry 3 -o "$dl" "$FXPLUG_SDK_URL"
echo "downloaded $(wc -c < "$dl" | tr -d ' ') bytes"
echo "${FXPLUG_SDK_SHA256}  $dl" | shasum -a 256 -c -

kind="$(file -b "$dl")"
echo "payload type: $kind"

install_pkgs_in() {
  local found=0
  while IFS= read -r -d '' pkg; do
    echo "installing $(basename "$pkg")"
    sudo installer -pkg "$pkg" -target / >/dev/null
    found=1
  done < <(find "$1" -maxdepth 4 \( -name '*.pkg' -o -name '*.mpkg' \) -print0)
  return $((1 - found))
}

copy_tree_from() {
  # A zipped /Library/Developer layout: <root>/Frameworks/FxPlug.framework
  local fw
  fw="$(find "$1" -type d -name FxPlug.framework -path '*/Frameworks/*' -prune -print | head -n 1)"
  [[ -n "$fw" ]] || return 1
  local root; root="$(dirname "$(dirname "$fw")")"
  echo "copying SDK tree from $root to /Library/Developer"
  sudo mkdir -p /Library/Developer
  sudo ditto "$root" /Library/Developer
}

case "$kind" in
  *"xar archive"*)  # flat .pkg
    mv "$dl" "$work/sdk.pkg"
    sudo installer -pkg "$work/sdk.pkg" -target / >/dev/null ;;
  *"Zip archive"*)
    mkdir -p "$work/zip"; ditto -x -k "$dl" "$work/zip"
    dmg="$(find "$work/zip" -maxdepth 4 -name '*.dmg' | head -n 1)"
    if [[ -n "$dmg" ]]; then
      mnt="$work/mnt"; mkdir -p "$mnt"
      hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
      install_pkgs_in "$mnt" || copy_tree_from "$mnt"
      hdiutil detach "$mnt" >/dev/null || true
    else
      install_pkgs_in "$work/zip" || copy_tree_from "$work/zip" || { echo "::error::no .pkg/.dmg/FxPlug.framework inside the zip"; exit 1; }
    fi ;;
  *)  # assume disk image (file(1) often says "data" or "zlib compressed data" for UDIF)
    mv "$dl" "$work/sdk.dmg"
    mnt="$work/mnt"; mkdir -p "$mnt"
    if ! hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$work/sdk.dmg" >/dev/null; then
      echo "::error::FXPLUG_SDK_URL payload is not a dmg/pkg/zip ($kind)"; exit 1
    fi
    install_pkgs_in "$mnt" || copy_tree_from "$mnt" || { echo "::error::no installer package in the dmg"; exit 1; }
    hdiutil detach "$mnt" >/dev/null || true ;;
esac

echo "Installed FxPlug SDK files:"
find /Library/Developer -maxdepth 5 \( -name 'FxPlug.framework' -o -name 'PluginManager.framework' -o -name 'FxPlug.sdk' \) -print 2>/dev/null || true
for fw in FxPlug PluginManager; do
  if [[ ! -d "/Library/Developer/Frameworks/$fw.framework" ]] && \
     [[ ! -d "/Library/Developer/SDKs/FxPlug.sdk/Library/Frameworks/$fw.framework" ]]; then
    echo "::error::$fw.framework not found under /Library/Developer after install"; exit 1
  fi
done
plist=/Library/Developer/Frameworks/FxPlug.framework/Resources/Info.plist
[[ -f "$plist" ]] && echo "FxPlug.framework version: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist" 2>/dev/null || echo '?')"
