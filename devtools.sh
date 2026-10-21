#!/bin/bash
# Development helpers for the emulator preview (ANDROID_SERIAL, default emulator-5580):
#   devtools.sh click X Y [r]   click (r: right-click) the window at its pixel (X, Y) (3200 x 2000)
#   devtools.sh panel OUT.png   the window's image (the virtual display is private: screencap cannot see it)
#   devtools.sh shot OUT.png    the screen (the preview)
S=${ANDROID_SERIAL:-emulator-5580}; A=${ANDROID_SDK_ROOT:-$HOME/Android/sdk}/platform-tools/adb; P=org.orcaport.slicer.xr
case $S in emulator-*) ;; *) echo "not an emulator: $S"; exit 1;; esac
case $1 in
  click) $A -s $S shell run-as $P sh -c "'echo $2 $3 $4 > files/click'" ;;
  panel) $A -s $S shell run-as $P touch files/dump_panel; sleep 3
         $A -s $S exec-out run-as $P cat files/panel.ppm > /tmp/oxr-panel.ppm
         python3 -c "from PIL import Image; Image.open('/tmp/oxr-panel.ppm').transpose(Image.FLIP_TOP_BOTTOM).save('$2')" ;;
  shot)  $A -s $S exec-out screencap -p > "$2" ;;
esac
