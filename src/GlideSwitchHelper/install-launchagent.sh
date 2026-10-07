#!/bin/bash
# Install (or remove) the LaunchAgent that starts GlideSwitchHelper at login.
# Usage: install-launchagent.sh            install and start
#        install-launchagent.sh --remove   stop and remove
# The agent runs the app from the repository root. Do not move the app after install.
set -euo pipefail
label=local.glideswitch.helper
plist="$HOME/Library/LaunchAgents/$label.plist"
domain="gui/$(id -u)"

if [[ "${1:-}" == "--remove" ]]; then
  launchctl bootout "$domain/$label" 2>/dev/null || true
  rm -f "$plist"
  echo "removed $plist"
  exit 0
fi

here="$(cd "$(dirname "$0")" && pwd)"
exe="$(cd "$here/../.." && pwd)/GlideSwitchHelper.app/Contents/MacOS/GlideSwitchHelper"
[[ -x "$exe" ]] || { echo "build the app first: $here/build.sh"; exit 1; }

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$exe</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>
  <key>LimitLoadToSessionType</key>
  <string>Aqua</string>
  <key>ProcessType</key>
  <string>Interactive</string>
  <key>StandardErrorPath</key>
  <string>$HOME/Library/Logs/GlideSwitchHelper.log</string>
</dict>
</plist>
EOF
plutil -lint "$plist" >/dev/null
launchctl bootout "$domain/$label" 2>/dev/null || true
launchctl bootstrap "$domain" "$plist"
echo "installed $plist"
echo "log: tail ~/Library/Logs/GlideSwitchHelper.log  (look for 'seize=1 result=0x00000000')"
