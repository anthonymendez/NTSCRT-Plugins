#!/usr/bin/env bash
# Syntax-check (no codegen, no link) the FxPlug plug-in's Objective-C++ against the stub
# FxPlug header in this folder, so CI catches typos and C++ errors without Apple's SDK.
# A pass does NOT mean the code matches the real SDK: see the header of FxPlug/FxPlugSDK.h.
#   fxplug/tests/stubs/syntax_check.sh <cmake build dir with FetchContent deps>   (macOS + Xcode)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="${1:-$root/build}"
json="$build/_deps/nlohmann_json-src/include"
if [ ! -f "$json/nlohmann/json.hpp" ]; then
  echo "syntax_check: nlohmann/json not found under $build/_deps (configure the build first)" >&2
  exit 2
fi
sdk="$(xcrun --sdk macosx --show-sdk-path)"
status=0
for f in NTSCRTPlugIn.mm MetalSupport.mm renderer_main.mm app_main.mm; do
  for arch in arm64 x86_64; do
    if xcrun clang++ -fsyntax-only -x objective-c++ -std=c++20 -fobjc-arc -arch "$arch" \
        -isysroot "$sdk" -mmacosx-version-min=12.0 \
        -Wall -Wextra -Wno-unused-parameter \
        -I "$here" -I "$root/core/include" -I "$root/fxplug/model" -I "$root/fxplug/src" -I "$json" \
        "$root/fxplug/src/$f"; then
      echo "ok   $f ($arch)"
    else
      echo "FAIL $f ($arch)"
      status=1
    fi
  done
done
exit $status
