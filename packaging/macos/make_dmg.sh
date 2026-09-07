#!/bin/sh
# make_dmg.sh - wrap a built OLEDGuard.app in a distributable disk image.
#
#   sh packaging/macos/make_dmg.sh [path/to/OLEDGuard.app] [output.dmg]
#
# Defaults to build/oledguard.app and OLEDGuard-<version>.dmg in the current
# directory. Needs nothing but macOS: the backdrop is committed as PNGs under
# packaging/macos/dmg/, and only re-rendered by make_dmg_bg.py after the SVG
# changes.
#
# The staging directory is built from scratch every time. Copying an existing
# folder instead would drag its .DS_Store into the image, which is how a disk
# image ends up carrying the icon positions and window size of whatever folder
# it was assembled in.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
APP=${1:-build/oledguard.app}
[ -d "$APP" ] || { echo "no app bundle at $APP" >&2; exit 1; }

VOL="OLEDGuard"
PB=/usr/libexec/PlistBuddy
VER=$("$PB" -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist" \
      2>/dev/null || echo 0.0.0)
OUT=${2:-OLEDGuard-$VER.dmg}

# Icon centres, in the coordinates of the 640x400 window opened below. These
# must match the plates drawn in dmg-background.svg.
APP_X=160; APP_Y=185
ALIAS_X=480; ALIAS_Y=185

STAGE=$(mktemp -d)
RW=$(mktemp -u).dmg
trap 'rm -rf "$STAGE" "$RW"' EXIT

mkdir -p "$STAGE/.background"
cp -R "$APP" "$STAGE/$VOL.app"
ln -s /Applications "$STAGE/Applications"

# One TIFF carrying both scales is how Finder picks the Retina backdrop; a
# lone @2x PNG is displayed at half size on a Retina display and blurred on
# every other.
tiffutil -cathidpicheck \
    "$HERE/dmg/background.png" "$HERE/dmg/background@2x.png" \
    -out "$STAGE/.background/background.tiff" >/dev/null

# Volume icon: the same artwork as the app, so the mounted disk and the app
# read as one thing in the Finder sidebar.
if [ -f "$HERE/OLEDGuard.icns" ]; then
    cp "$HERE/OLEDGuard.icns" "$STAGE/.VolumeIcon.icns"
fi

echo "staging $VOL $VER"
hdiutil create -quiet -srcfolder "$STAGE" -volname "$VOL" \
    -fs HFS+ -fsargs "-c c=64,a=16,e=16" -format UDRW -ov "$RW"

MNT=$(hdiutil attach -readwrite -noverify -noautoopen "$RW" \
      | grep -o '/Volumes/.*' | head -1)
[ -n "$MNT" ] || { echo "could not mount the staging image" >&2; exit 1; }

# The volume icon only takes effect once the volume carries the custom-icon
# attribute. SetFile ships with the command line tools; without it the image
# still builds and simply keeps the generic disk icon.
if command -v SetFile >/dev/null 2>&1 && [ -f "$MNT/.VolumeIcon.icns" ]; then
    SetFile -a C "$MNT"
fi

osascript <<EOF >/dev/null
tell application "Finder"
  tell disk "$VOL"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 840, 520}
    set opts to the icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 12
    set label position of opts to bottom
    set background picture of opts to file ".background:background.tiff"
    set position of item "$VOL.app" of container window to {$APP_X, $APP_Y}
    set position of item "Applications" of container window to {$ALIAS_X, $ALIAS_Y}
    close
    open
    update without registering applications
    delay 2
  end tell
end tell
EOF

# Finder writes the window settings to .DS_Store lazily; give it a moment
# before pulling the volume out from underneath it.
sync
hdiutil detach "$MNT" -quiet || { sleep 3; hdiutil detach "$MNT" -quiet; }

rm -f "$OUT"
hdiutil convert -quiet "$RW" -format UDZO -imagekey zlib-level=9 -o "$OUT"

echo "wrote $OUT"
echo
echo "This image is not notarised. On another Mac the first launch is refused;"
echo "the user allows it once in System Settings > Privacy & Security."
