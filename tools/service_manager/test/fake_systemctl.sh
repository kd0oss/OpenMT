#!/bin/sh
# Test stand-in for systemctl: logs the arguments, answers "show" like systemd does.
echo "$@" >> "$SM_LOG"
if [ "$1" = "show" ]; then
    shift
    while [ "$1" != "--" ]; do shift; done
    shift
    first=1
    for u in "$@"; do
        [ $first = 0 ] && echo
        first=0
        case "$u" in
            fm-host.service) echo "Id=$u"; echo "ActiveState=active"; echo "UnitFileState=enabled" ;;
            fm-gateway.service) echo "Id=$u"; echo "ActiveState=inactive"; echo "UnitFileState=disabled" ;;
            missing.service) echo "Id=$u"; echo "LoadState=not-found"; echo "ActiveState=inactive"; echo "UnitFileState=" ;;
            *) echo "Id=$u"; echo "ActiveState=inactive"; echo "UnitFileState=" ;;
        esac
    done
    exit 0
fi
case "$*" in *fail.service*) exit 5 ;; esac
exit 0
