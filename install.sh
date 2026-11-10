#!/bin/sh
# NTSCRT installer for macOS: DaVinci Resolve (OpenFX) and/or Final Cut Pro (FxPlug).
#
# One line, in Terminal:
#   curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh
#
# Options (pass after `sh -s --` when piping):
#   curl -fsSL .../install.sh | sh -s -- --resolve          # only DaVinci Resolve
#   curl -fsSL .../install.sh | sh -s -- --fcp              # only Final Cut Pro
#   curl -fsSL .../install.sh | sh -s -- --version v0.3.0   # a specific release
#   sh install.sh --from ~/Downloads/NTSCRT-OFX-macos-universal.zip   # a zip you already have
#   sh install.sh --uninstall
#
# Default: installs for every app it finds (/Applications/DaVinci Resolve,
# /Applications/Final Cut Pro.app), or for both if it finds neither.
# Asks for your password once (sudo) to write /Library/OFX/Plugins and /Applications.
# If the direct download fails it falls back to `gh release download`
# (GitHub CLI, signed in) or the GITHUB_TOKEN environment variable.
set -eu

REPO="${NTSCRT_REPO:-anthonymendez/NTSCRT-Plugins}"
VERSION="latest"
WANT_RESOLVE=""; WANT_FCP=""; UNINSTALL=""; FROM=""
OFX_ASSET="NTSCRT-OFX-macos-universal.zip"
FCP_ASSET="NTSCRT-FxPlug-macos-universal.zip"
OFX_DEST="/Library/OFX/Plugins"
APP_DEST="/Applications"
TEMPLATE_DIR="$HOME/Movies/Motion Templates.localized/Effects.localized/NTSCRT"

say() { printf '\033[36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --resolve) WANT_RESOLVE=1 ;;
    --fcp|--finalcut) WANT_FCP=1 ;;
    --all) WANT_RESOLVE=1; WANT_FCP=1 ;;
    --version) shift; VERSION="${1:?--version needs a tag}" ;;
    --from) shift; FROM="${1:?--from needs a path}" ;;
    --repo) shift; REPO="${1:?--repo needs owner/name}" ;;
    --uninstall) UNINSTALL=1 ;;
    -h|--help) sed -n '2,20p' "$0" 2>/dev/null || true; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

[ "$(uname -s)" = "Darwin" ] || die "this installer is for macOS; on Windows use install.ps1"

# Run from an unzipped release (script file next to the plugin)? Install that copy.
if [ -z "$FROM" ] && [ -f "$0" ]; then
  here="$(cd "$(dirname "$0")" && pwd)"
  if [ -d "$here/NTSCRT.ofx.bundle" ] || [ -d "$here/NTSCRT.app" ]; then FROM="$here"; fi
fi

# Nothing chosen: pick by what's installed (both if neither is found).
if [ -z "$WANT_RESOLVE$WANT_FCP" ]; then
  [ -d "/Applications/DaVinci Resolve" ] || [ -d "/Applications/DaVinci Resolve.app" ] && WANT_RESOLVE=1
  [ -d "/Applications/Final Cut Pro.app" ] && WANT_FCP=1
  [ -z "$WANT_RESOLVE$WANT_FCP" ] && { WANT_RESOLVE=1; WANT_FCP=1; }
fi

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

# Running as root (the .pkg installer's postinstall, or sudo)? The Final Cut steps
# (opening NTSCRT.app, pluginkit, the effect template in ~/Movies) are per user:
# run them as the user logged in at the screen, in their GUI session.
CONSOLE_USER=""
if [ "$(id -u)" -eq 0 ]; then
  CONSOLE_USER="$(stat -f%Su /dev/console 2>/dev/null || true)"
  case "$CONSOLE_USER" in ""|root|loginwindow) CONSOLE_USER="" ;; esac
fi
if [ -n "$CONSOLE_USER" ]; then
  user_home="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
  TEMPLATE_DIR="${user_home:-/Users/$CONSOLE_USER}/Movies/Motion Templates.localized/Effects.localized/NTSCRT"
fi
as_user() {  # as_user <cmd...>: run as the console user when we're root
  if [ -n "$CONSOLE_USER" ]; then
    launchctl asuser "$(id -u "$CONSOLE_USER")" sudo -u "$CONSOLE_USER" "$@"
  else
    "$@"
  fi
}

if [ -n "$UNINSTALL" ]; then
  if [ -n "$WANT_RESOLVE" ]; then
    say "Removing $OFX_DEST/NTSCRT.ofx.bundle"
    $SUDO rm -rf "$OFX_DEST/NTSCRT.ofx.bundle" "$OFX_DEST/.NTSCRT.ofx.bundle.new-"* "$OFX_DEST/.NTSCRT.ofx.bundle.old-"*
  fi
  if [ -n "$WANT_FCP" ]; then
    say "Removing $APP_DEST/NTSCRT.app and its effect template"
    $SUDO rm -rf "$APP_DEST/NTSCRT.app"
    as_user rm -rf "$TEMPLATE_DIR"
  fi
  say "Done."
  exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ntscrt.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

download() {  # download <asset> -> prints path of the zip
  asset="$1"; out="$WORK/$asset"
  if [ "$VERSION" = "latest" ]; then url="https://github.com/$REPO/releases/latest/download/$asset"
  else url="https://github.com/$REPO/releases/download/$VERSION/$asset"; fi
  say "Downloading $asset ($VERSION) from $REPO" >&2
  if curl -fsSL -o "$out" "$url"; then echo "$out"; return 0; fi
  if command -v gh >/dev/null 2>&1; then
    say "Trying GitHub CLI (gh release download)" >&2
    if [ "$VERSION" = "latest" ]; then set -- release download; else set -- release download "$VERSION"; fi
    if gh "$@" --repo "$REPO" --pattern "$asset" --dir "$WORK" --clobber >&2; then echo "$out"; return 0; fi
  fi
  if [ -n "${GITHUB_TOKEN:-}" ]; then
    say "Trying the GitHub API with GITHUB_TOKEN" >&2
    if [ "$VERSION" = "latest" ]; then rel="latest"; else rel="tags/$VERSION"; fi
    api_url="$(curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" "https://api.github.com/repos/$REPO/releases/$rel" |
      /usr/bin/python3 -c 'import json,sys; a=[x["url"] for x in json.load(sys.stdin)["assets"] if x["name"]==sys.argv[1]]; print(a[0] if a else "")' "$asset")"
    if [ -n "$api_url" ] && curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/octet-stream" -o "$out" "$api_url"; then
      echo "$out"; return 0
    fi
  fi
  die "couldn't download $asset. Check https://github.com/$REPO/releases or use --from <zip>."
}

unpack() {  # unpack <zip|dir> <name to find> -> prints path of the found item
  src="$1"; name="$2"
  if [ -d "$src" ]; then
    case "$src" in */"$name") echo "$src"; return 0 ;; esac
    found="$(find "$src" -maxdepth 3 -name "$name" -type d | head -n 1)"
  else
    x="$WORK/x.$$.$(basename "$src")"; mkdir -p "$x"
    ditto -x -k "$src" "$x"
    # GitHub Actions artifacts wrap the zip in another zip: unwrap one level.
    if ! find "$x" -maxdepth 3 -name "$name" -type d | grep -q . ; then
      for inner in "$x"/*.zip; do [ -f "$inner" ] && ditto -x -k "$inner" "$x/inner"; done
    fi
    found="$(find "$x" -maxdepth 4 -name "$name" -type d | head -n 1)"
  fi
  [ -n "$found" ] || die "no $name inside $src"
  echo "$found"
}

pick_from() {  # pick_from <name>: with --from, use it only if it contains <name>
  [ -n "$FROM" ] || return 1
  [ -e "$FROM" ] || die "--from: $FROM doesn't exist"
  unpack "$FROM" "$1" 2>/dev/null
}

bundle_version() {  # bundle_version <NTSCRT.ofx.bundle | NTSCRT.app> -> "v0.3.0" or ""
  if [ -f "$1/Contents/Resources/VERSION" ]; then printf 'v%s' "$(head -n 1 "$1/Contents/Resources/VERSION" | tr -d '[:space:]')"
  elif [ -f "$1/Contents/Info.plist" ]; then
    v="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$1/Contents/Info.plist" 2>/dev/null || true)"
    [ -n "$v" ] && printf 'v%s' "$v"
  fi
  return 0
}

# install_swap <source> <dest dir> <name>: atomic replace of <dest dir>/<name>.
# The new copy is staged next to the old one (and de-quarantined / signed there),
# then the two are swapped with mv, rolling back if the swap fails. A failed copy
# never touches the installed version. The staging names (.<name>.new-PID /
# .<name>.old-PID) don't end in .ofx.bundle / .app, so no host loads them.
install_swap() {
  src="$1"; dir="$2"; name="$3"
  new="$dir/.$name.new-$$"; old="$dir/.$name.old-$$"
  $SUDO mkdir -p "$dir"
  $SUDO rm -rf "$dir/.$name.new-"* "$dir/.$name.old-"* 2>/dev/null || true   # leftovers of an interrupted run
  if ! $SUDO ditto "$src" "$new"; then $SUDO rm -rf "$new"; die "copy to $dir failed; the installed version is untouched"; fi
  $SUDO xattr -dr com.apple.quarantine "$new" 2>/dev/null || true
  case "$name" in *.ofx.bundle) $SUDO codesign --force --deep --sign - "$new" 2>/dev/null || true ;; esac
  if [ -e "$dir/$name" ] && ! $SUDO mv "$dir/$name" "$old"; then
    $SUDO rm -rf "$new"; die "couldn't move the old $name aside; it is untouched"
  fi
  if ! $SUDO mv "$new" "$dir/$name"; then
    [ -e "$old" ] && $SUDO mv "$old" "$dir/$name"   # roll back
    $SUDO rm -rf "$new"; die "couldn't put the new $name in place; the previous version was restored"
  fi
  $SUDO rm -rf "$old"
}

# With --from, install only what the given zip/folder contains (never download).
if [ -n "$FROM" ]; then
  if [ -n "$WANT_RESOLVE" ] && ! (pick_from NTSCRT.ofx.bundle) >/dev/null 2>&1; then
    say "No Resolve plugin in $FROM: skipping Resolve."; WANT_RESOLVE=""
  fi
  if [ -n "$WANT_FCP" ] && ! (pick_from NTSCRT.app) >/dev/null 2>&1; then
    say "No Final Cut Pro app in $FROM: skipping Final Cut Pro."; WANT_FCP=""
  fi
  [ -n "$WANT_RESOLVE$WANT_FCP" ] || die "nothing to install from $FROM"
fi

if [ -n "$SUDO" ]; then
  say "Administrator password needed to install into $OFX_DEST / $APP_DEST"
  sudo -v || die "sudo failed"
fi

if [ -n "$WANT_RESOLVE" ]; then
  bundle="$(pick_from NTSCRT.ofx.bundle || unpack "$(download "$OFX_ASSET")" NTSCRT.ofx.bundle)"
  [ -f "$bundle/Contents/MacOS/NTSCRT.ofx" ] || die "not a macOS NTSCRT bundle: $bundle"
  if pgrep -xq Resolve; then say "DaVinci Resolve is running: the new version loads when you restart it."; fi
  new_v="$(bundle_version "$bundle")"; old_v="$(bundle_version "$OFX_DEST/NTSCRT.ofx.bundle")"
  say "Installing DaVinci Resolve plugin $new_v${old_v:+ (replacing $old_v)} -> $OFX_DEST/NTSCRT.ofx.bundle"
  install_swap "$bundle" "$OFX_DEST" NTSCRT.ofx.bundle
  say "Resolve: (re)start it, then Effects > OpenFX > Filters > NTSCRT."
fi

if [ -n "$WANT_FCP" ]; then
  app="$(pick_from NTSCRT.app || unpack "$(download "$FCP_ASSET")" NTSCRT.app)"
  if pgrep -xq "Final Cut Pro"; then say "Final Cut Pro is running: the new version loads when you restart it."; fi
  new_v="$(bundle_version "$app")"; old_v="$(bundle_version "$APP_DEST/NTSCRT.app")"
  say "Installing Final Cut Pro plugin $new_v${old_v:+ (replacing $old_v)} -> $APP_DEST/NTSCRT.app"
  install_swap "$app" "$APP_DEST" NTSCRT.app
  # Opening the app once registers the plug-in and installs the effect template; it quits by itself.
  say "Registering with Final Cut Pro (opens NTSCRT.app once)"
  as_user open -g "$APP_DEST/NTSCRT.app" 2>/dev/null || as_user open "$APP_DEST/NTSCRT.app" || true
  registered=""
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if as_user pluginkit -mA -p FxPlug 2>/dev/null | grep -qi ntscrt; then registered=1; break; fi
    sleep 1
  done
  if [ -n "$registered" ]; then
    say "Final Cut Pro: restart it, then Effects browser > NTSCRT."
  else
    say "Installed, but the plug-in isn't registered yet: open /Applications/NTSCRT.app once yourself, then restart Final Cut Pro."
  fi
fi

say "Done."
