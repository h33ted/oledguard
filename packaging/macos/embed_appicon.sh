#!/bin/sh
# Compile packaging/macos/AppIcon.icon (Icon Composer, Xcode 26) into the built
# bundle, giving macOS 26 layered Liquid Glass artwork to render rather than an
# automatic treatment of a flat PNG.
#
# OLEDGuard.icns stays in Resources regardless. That is not a competing
# mechanism: CFBundleIconFile is what every macOS before 26 reads, and
# CFBundleIconName (this) takes precedence on 26 and later.
#
# actool ships with full Xcode, not the Command Line Tools. Without it this
# exits quietly and the .icns carries the whole load. Never fails the build.
#
#   embed_appicon.sh <path/to/App.app> <path/to/AppIcon.icon> <min-os>
set -u

APP="${1:-}"
ICON="${2:-}"
MINOS="${3:-12.3}"

[ -d "$APP" ]  || { echo "note: no bundle at $APP, skipping icon"; exit 0; }
[ -d "$ICON" ] || { echo "note: no AppIcon.icon, using OLEDGuard.icns"; exit 0; }

if ! xcrun --find actool >/dev/null 2>&1; then
    echo "note: actool unavailable (Command Line Tools only)."
    echo "      Using OLEDGuard.icns. Point xcode-select at full Xcode for the"
    echo "      layered macOS 26 icon."
    exit 0
fi

TMP=$(mktemp -d) || exit 0
RES="$APP/Contents/Resources"
PLIST="$APP/Contents/Info.plist"
PB=/usr/libexec/PlistBuddy
OK=""

# .icon is new enough that the exact actool invocation is worth probing rather
# than asserting. Each attempt logs its own failure, so a build.log says which
# form this toolchain accepts instead of leaving a silent fallback.
try_actool() {
    label="$1"; shift
    if xcrun actool "$@" >"$TMP/out" 2>&1; then
        echo "icon: compiled via $label"
        OK=1
        return 0
    fi
    echo "icon: $label did not work:"
    sed 's/^/      /' "$TMP/out" | head -6
    return 1
}

# 1. the .icon handed to actool directly, at the project's deployment target
try_actool "actool <AppIcon.icon>, min $MINOS" \
    "$ICON" --compile "$RES" --platform macosx \
    --minimum-deployment-target "$MINOS" --target-device mac \
    --app-icon AppIcon --output-partial-info-plist "$TMP/partial.plist"

# 2. same, but declaring macOS 26: layered icons may be refused below it
[ -z "$OK" ] && try_actool "actool <AppIcon.icon>, min 26.0" \
    "$ICON" --compile "$RES" --platform macosx \
    --minimum-deployment-target 26.0 --target-device mac \
    --app-icon AppIcon --output-partial-info-plist "$TMP/partial.plist"

# 3. wrapped in an asset catalog, which is how Xcode itself lays it out
if [ -z "$OK" ]; then
    CAT="$TMP/Assets.xcassets"
    mkdir -p "$CAT"
    printf '{"info":{"author":"oledguard","version":1}}\n' > "$CAT/Contents.json"
    cp -R "$ICON" "$CAT/" 2>/dev/null
    try_actool "actool <Assets.xcassets containing AppIcon.icon>" \
        "$CAT" --compile "$RES" --platform macosx \
        --minimum-deployment-target 26.0 --target-device mac \
        --app-icon AppIcon --output-partial-info-plist "$TMP/partial.plist"
fi

if [ -n "$OK" ]; then
    # actool reports the icon name it actually registered; trust that over a
    # guess, since it need not match the file name.
    NAME=$("$PB" -c "Print :CFBundleIconName" "$TMP/partial.plist" 2>/dev/null)
    [ -n "$NAME" ] || NAME=AppIcon
    "$PB" -c "Set :CFBundleIconName $NAME" "$PLIST" 2>/dev/null \
      || "$PB" -c "Add :CFBundleIconName string $NAME" "$PLIST" 2>/dev/null
    echo "icon: CFBundleIconName = $NAME, Assets.car in Resources"
else
    echo "icon: no actool form succeeded; OLEDGuard.icns remains in charge"
fi

rm -rf "$TMP"
exit 0
