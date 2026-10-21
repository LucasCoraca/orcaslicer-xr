#!/bin/bash
# Builds OrcaSlicer XR: OrcaSlicer for Android's app build with this checkout added (its android/app/CMakeLists.txt adds
# the target orcaslicer_xr when ORCA_XR_DIR is set), then the APK, signed, into dist/.
#   usage: ./build.sh [arm64-v8a] [x86_64]          (default: both; arm64-v8a = the headset, x86_64 = the emulator preview)
#   ORCA_ANDROID: a checkout of OrcaSlicer for Android with its dependencies built (see its README); default: the
#                 orcaslicer-android submodule
set -e
X="$(cd "$(dirname "$0")" && pwd)"
O=${ORCA_ANDROID:-$X/orcaslicer-android}
mkdir -p "$X/dist"
for abi in ${@:-arm64-v8a x86_64}; do
  source "$O/env.sh"
  [ -f "$O/build/app-$abi/CMakeCache.txt" ] || "$O/configure-app.sh" "$abi"
  cmake -DORCA_XR_DIR="$X" "$O/build/app-$abi" > /dev/null
  "$O/build-app.sh" "$abi" orcaslicer_xr_make_apk
  apk=$(ls -t "$O/build/app-$abi"/xr-app/android-build/*.apk "$O/build/app-$abi"/xr-app/android-build/build/outputs/apk/*/*.apk 2>/dev/null | head -1)
  short=${abi/-v8a/}
  name=$([ "$abi" = arm64-v8a ] && echo quest || echo emulator-$short)
  "$BT/apksigner" sign --ks "$ORCA_KEYSTORE" --ks-pass pass:android --key-pass pass:android --out "$X/dist/OrcaSlicer-XR-$name.apk" "$apk"
  echo "dist/OrcaSlicer-XR-$name.apk"
done
