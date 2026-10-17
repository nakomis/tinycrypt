#!/usr/bin/env bash
# CRYPT-30: App Store screenshots of the iPhone/iPad app, from the simulators, with made-up data
# (the debug build's TC_SCREENSHOT mode: iOS/ScreenshotView.swift).
#
#   watch/ci/screenshots.sh OUT_DIR
#
# iPhone shots are resized to 1284x2778 (App Store Connect's 6.5" slot); iPad Pro 13" is taken as is
# (2064x2752), and the watch on a 46 mm Series (416x496, its main screen with a key added), flattened.
set -euo pipefail
out=${1:?usage: screenshots.sh OUT_DIR}
mkdir -p "$out"
out=$(cd "$out" && pwd)
here=$(cd "$(dirname "$0")/../TinycryptWatch" && pwd)
dd=$(mktemp -d)
trap 'rm -rf "$dd"' EXIT

cd "$here"
xcodegen generate > /dev/null
xcodebuild -project TinycryptWatch.xcodeproj -scheme TinycryptWatch -configuration Debug \
    -destination 'generic/platform=iOS Simulator' -derivedDataPath "$dd" CODE_SIGNING_ALLOWED=NO build > "$dd/build.log"
app="$dd/Build/Products/Debug-iphonesimulator/tinycrypt.app"

shoot() {  # device-name file-prefix resize
    local name=$1 prefix=$2 resize=$3
    local id
    id=$(xcrun simctl list devices available -j | python3 -c "
import json, sys
devs = [d for rt in json.load(sys.stdin)['devices'].values() for d in rt if d['name'] == sys.argv[1]]
print(devs[-1]['udid'])" "$name")
    xcrun simctl boot "$id" 2> /dev/null || true
    xcrun simctl bootstatus "$id" -b > /dev/null
    xcrun simctl status_bar "$id" override --time 9:41 --batteryState charged --batteryLevel 100 \
        --wifiBars 3 --cellularBars 4 --operatorName ""
    xcrun simctl install "$id" "$app"
    local n=1
    for screen in list approve add wifi; do
        SIMCTL_CHILD_TC_SCREENSHOT=$screen xcrun simctl launch --terminate-running-process "$id" \
            com.nakomis.tinycrypt.watch > /dev/null
        sleep 4
        xcrun simctl io "$id" screenshot "$out/$prefix-$n-$screen.png" > /dev/null
        [ -n "$resize" ] && sips -z ${resize#*x} ${resize%x*} "$out/$prefix-$n-$screen.png" > /dev/null
        n=$((n + 1))
    done
    xcrun simctl terminate "$id" com.nakomis.tinycrypt.watch 2> /dev/null || true
    xcrun simctl shutdown "$id"
}

shoot "iPhone 17 Pro Max" iphone 1284x2778
shoot "iPad Pro 13-inch (M5)" ipad ""

# The watch app: built on its own for the watch simulator.
xcodebuild -project TinycryptWatch.xcodeproj -scheme TinycryptWatchApp -configuration Debug \
    -destination 'generic/platform=watchOS Simulator' -derivedDataPath "$dd" CODE_SIGNING_ALLOWED=NO build >> "$dd/build.log"
watch=$(xcrun simctl list devices available -j | python3 -c "
import json, sys
devs = [d for rt, ds in json.load(sys.stdin)['devices'].items() if 'watchOS' in rt for d in ds
        if 'Series' in d['name'] and '46mm' in d['name']]
print(devs[-1]['udid'])")
xcrun simctl boot "$watch" 2> /dev/null || true
xcrun simctl bootstatus "$watch" -b > /dev/null
xcrun simctl install "$watch" "$dd/Build/Products/Debug-watchsimulator/TinycryptWatch.app"
SIMCTL_CHILD_TC_SCREENSHOT=1 xcrun simctl launch --terminate-running-process "$watch" \
    com.nakomis.tinycrypt.watch.watchkitapp > /dev/null
sleep 5
xcrun simctl io "$watch" screenshot "$out/watch-raw.png" > /dev/null
# App Store Connect rejects the simulator's transparent corners: flatten onto black.
ffmpeg -v error -y -i "$out/watch-raw.png" -filter_complex "color=black[bg];[bg][0:v]scale2ref[bg][fg];[bg][fg]overlay=format=auto,format=rgb24" \
    -frames:v 1 "$out/watch-1-added.png"
rm "$out/watch-raw.png"
xcrun simctl shutdown "$watch"
ls -1 "$out"
