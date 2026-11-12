#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs as root when you choose Remove Background Helper. Quitting the helper makes it
# remove its network service and virtual ports first.
label=local.mytether.helperd

/bin/launchctl bootout "system/$label" 2>/dev/null
/bin/rm -f "/Library/LaunchDaemons/$label.plist"
/bin/rm -rf "/Library/PrivilegedHelperTools/$label"
/bin/rm -f /var/log/mytether-helper.log
exit 0
