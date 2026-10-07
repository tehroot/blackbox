#!/bin/bash
# Build GlideSwitchHelper.app into the parent "blackbox config" folder.
# Note: an ad-hoc signature changes on each build. macOS then asks again for
# Input Monitoring and Accessibility.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app="$here/../../GlideSwitchHelper.app"
mkdir -p "$app/Contents/MacOS"
swiftc -O -o "$app/Contents/MacOS/GlideSwitchHelper" "$here/main.swift"
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.glideswitch.helper</string>
<key>CFBundleName</key><string>GlideSwitchHelper</string>
<key>CFBundleExecutable</key><string>GlideSwitchHelper</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSUIElement</key><true/>
</dict></plist>
EOF
codesign -s - --force --deep "$app"
echo "built $app"
