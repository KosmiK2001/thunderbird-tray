#!/bin/sh
# Собирает tb-tray-extension.xpi (MailExtension для Thunderbird)
cd "$(dirname "$0")/../extension"
exec zip -q -r "$OLDPWD/tb-tray-extension.xpi" . -x '.*'
