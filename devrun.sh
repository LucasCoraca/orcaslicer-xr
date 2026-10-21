#!/bin/bash
# Development loop on the emulator (x86_64): rebuild the XR APK, sign, install, launch, show the log.
# usage: ./devrun.sh [seconds to wait before the log]   ORCA_ANDROID: the built OrcaSlicer for Android checkout
set -e
X="$(cd "$(dirname "$0")" && pwd)"; O=${ORCA_ANDROID:-$X/orcaslicer-android}
SERIAL=${ANDROID_SERIAL:-emulator-5580}
case $SERIAL in emulator-*) ;; *) echo "not an emulator: $SERIAL"; exit 1;; esac
source "$O/env.sh"
JOBS=${JOBS:-8} "$O/build-app.sh" x86_64 orcaslicer_xr_make_apk > /tmp/oxr-build.log 2>&1 || { grep -E "error|FAILED" /tmp/oxr-build.log | head; exit 1; }
mkdir -p "$X/dist"
"$BT/apksigner" sign --ks "$ORCA_KEYSTORE" --ks-pass pass:android --key-pass pass:android --out "$X/dist/dev.apk" "$O/build/app-x86_64/xr-app/android-build/orcaslicer_xr.apk"
A=$ANDROID_SDK_ROOT/platform-tools/adb
$A -s $SERIAL install -r "$X/dist/dev.apk" | tail -1
$A -s $SERIAL logcat -c
$A -s $SERIAL shell am start -n org.orcaport.slicer.xr/org.orcaport.xr.XrActivity > /dev/null
sleep ${1:-45}
$A -s $SERIAL logcat -d | grep -E "orcaxr|FATAL|AndroidRuntime: |Fatal signal|DEBUG.*#[0-9]" | head -40
