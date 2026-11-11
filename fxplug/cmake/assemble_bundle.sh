#!/bin/bash
# Assemble and sign the FxPlug 4 bundle (called by the ntscrt_fxplug target):
#
#   NTSCRT.app/Contents/
#     MacOS/NTSCRT                       wrapper app (installs the Motion template)
#     Resources/Motion Templates/NTSCRT/NTSCRT.moef
#     PlugIns/NTSCRT Renderer.pluginkit/Contents/
#       Info.plist                       (CFBundlePackageType XPC!)
#       MacOS/NTSCRTRenderer             the FxPlug plug-in (FxTileableEffect)
#       Frameworks/FxPlug.framework, PluginManager.framework   (from the SDK)
#       Resources/presets, shaders, param_ids.json,
#                 LICENSE, THIRD_PARTY_NOTICES.md, licenses/
#
# Signing is inside-out: frameworks, renderer (sandbox entitlements), app.
# Identity "-" = ad-hoc (local testing; no hardened runtime so ad-hoc library
# validation can't reject the embedded frameworks). Any other identity signs
# with --options runtime --timestamp, ready for notarization.
set -euo pipefail

# Arguments are --key=value so an empty value (e.g. no shaders dir) can't shift the rest.
SHADERS=""
LIBRASHADER=""
LICENSE_ROOT=""
for arg in "$@"; do
  v="${arg#*=}"
  case "$arg" in
    --app=*) APP="$v" ;; --renderer=*) RENDERER="$v" ;; --plugin-name=*) PLUGIN_NAME="$v" ;;
    --plist=*) PLIST="$v" ;; --frameworks=*) FRAMEWORKS="$v" ;; --presets=*) PRESETS="$v" ;;
    --shaders=*) SHADERS="$v" ;; --librashader=*) LIBRASHADER="$v" ;; --param-ids=*) PARAM_IDS="$v" ;; --license-root=*) LICENSE_ROOT="$v" ;; --template-dir=*) TEMPLATE_DIR="$v" ;;
    --renderer-entitlements=*) RENT="$v" ;; --app-entitlements=*) AENT="$v" ;; --identity=*) IDENTITY="$v" ;;
    *) echo "assemble_bundle.sh: unknown arg $arg" >&2; exit 2 ;;
  esac
done

PK="$APP/Contents/PlugIns/$PLUGIN_NAME.pluginkit"
rm -rf "$PK"
mkdir -p "$PK/Contents/MacOS" "$PK/Contents/Frameworks" "$PK/Contents/Resources"
cp "$PLIST" "$PK/Contents/Info.plist"
printf 'XPC!????' > "$PK/Contents/PkgInfo"
cp "$RENDERER" "$PK/Contents/MacOS/"
for fw in FxPlug PluginManager; do
  src="$FRAMEWORKS/$fw.framework"
  [[ -d "$src" ]] || { echo "error: $src missing" >&2; exit 1; }
  # keep symlinks (Versions/Current), drop headers like Apple's embed phase
  rsync -a --exclude 'Headers' --exclude 'PrivateHeaders' --exclude 'Modules' "$src" "$PK/Contents/Frameworks/"
done
rsync -a --delete "$PRESETS/" "$PK/Contents/Resources/presets/"
if [[ -n "${SHADERS:-}" && -d "$SHADERS" ]]; then
  rsync -a --delete "$SHADERS/" "$PK/Contents/Resources/shaders/"
else
  echo "note: no shaders dir given (NTSCRT_SHADERS_DIR) - CRT stage will report missing resources" >&2
fi
cp "$PARAM_IDS" "$PK/Contents/Resources/param_ids.json"
if [[ -n "$LICENSE_ROOT" ]]; then
  cp "$LICENSE_ROOT/LICENSE" "$LICENSE_ROOT/THIRD_PARTY_NOTICES.md" "$PK/Contents/Resources/"
  rsync -a --delete "$LICENSE_ROOT/licenses/" "$PK/Contents/Resources/licenses/"
  # The wrapper app is what users see in /Applications: give it the same texts.
  mkdir -p "$APP/Contents/Resources"
  cp "$LICENSE_ROOT/LICENSE" "$LICENSE_ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/"
  rsync -a --delete "$LICENSE_ROOT/licenses/" "$APP/Contents/Resources/licenses/"
fi
if [ -n "${LIBRASHADER:-}" ] && [ -f "$LIBRASHADER" ]; then
  cp "$LIBRASHADER" "$PK/Contents/Frameworks/"   # CrtStage searches <Resources>/../Frameworks
fi
mkdir -p "$APP/Contents/Resources/Motion Templates"
rsync -a --delete "$TEMPLATE_DIR/" "$APP/Contents/Resources/Motion Templates/"

# Universal check: warn if the renderer or frameworks miss an architecture.
for bin in "$PK/Contents/MacOS/$(basename "$RENDERER")" "$PK/Contents/Frameworks/FxPlug.framework/FxPlug"; do
  [[ -e "$bin" ]] && echo "$(basename "$bin"): $(lipo -archs "$bin" 2>/dev/null || echo '?')"
done

sign() {  # sign <path> [entitlements]
  local args=(--force --sign "$IDENTITY")
  if [[ "$IDENTITY" != "-" ]]; then args+=(--options runtime --timestamp); fi
  if [[ -n "${2:-}" ]]; then args+=(--entitlements "$2"); fi
  codesign "${args[@]}" "$1"
}
for fw in "$PK"/Contents/Frameworks/*.framework "$PK"/Contents/Frameworks/*.dylib; do
  [[ -e "$fw" ]] && sign "$fw"
done
sign "$PK" "$RENT"
sign "$APP" "$AENT"
codesign --verify --deep --strict "$APP"
echo "assembled $APP (signed: $IDENTITY)"
