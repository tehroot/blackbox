#!/bin/bash
# Build GSCap.app (HID capture tool) into the repository root.
# The app bundle gets its own Input Monitoring permission, so Terminal does not need it.
# Note: an ad-hoc signature changes on each build. macOS then asks again for the permission.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app="$here/../../GSCap.app"
mkdir -p "$app/Contents/MacOS"
swiftc -O -o "$app/Contents/MacOS/gscap" "$here/main.swift"
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.glideswitch.gscap</string>
<key>CFBundleName</key><string>GSCap</string>
<key>CFBundleExecutable</key><string>gscap</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSUIElement</key><true/>
</dict></plist>
EOF
codesign -s - --force --deep "$app"
echo "built $app"
echo "run:  open -W -n \"$app\" --args 90 /tmp/gscap.log"
