#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs as root once, when you click Set Up or Update. It's also what cleans up older versions.
set -eu

app=$1
label=local.mytether.helperd
folder=/Library/PrivilegedHelperTools/$label
plist=/Library/LaunchDaemons/$label.plist

/usr/bin/codesign --verify "$app"
fingerprint=$(/usr/bin/codesign -dv --verbose=4 "$app" 2>&1 | /usr/bin/sed -n 's/^CDHash=//p')
[ -n "$fingerprint" ]

/bin/launchctl bootout "system/$label" 2>/dev/null || true
/bin/rm -rf "$folder"
/bin/rm -f /var/log/mytether-helper.log

/bin/mkdir -p "$folder"
/usr/bin/install -m 755 -o root -g wheel "$app/Contents/MacOS/mytether-helper" "$folder/mytether-helper"
printf '%s\n' "$fingerprint" > "$folder/app-cdhash"
/bin/chmod 644 "$folder/app-cdhash"

# No RunAtLoad or KeepAlive: macOS starts the helper only when the app calls it. No log
# paths either, so its output goes nowhere.
/bin/cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$label</string>
    <key>ProgramArguments</key>
    <array>
        <string>$folder/mytether-helper</string>
    </array>
    <key>MachServices</key>
    <dict>
        <key>$label</key>
        <true/>
    </dict>
    <key>ProcessType</key>
    <string>Interactive</string>
</dict>
</plist>
PLIST
/usr/sbin/chown root:wheel "$plist"
/bin/chmod 644 "$plist"

# Removing the old helper finishes a moment after the command returns.
for attempt in 1 2 3 4 5 6 7 8; do
    /bin/launchctl bootstrap system "$plist" 2>/dev/null && exit 0
    /bin/sleep 1
done
exit 1
