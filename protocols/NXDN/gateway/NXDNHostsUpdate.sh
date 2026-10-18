#!/bin/sh
# Downloads the NXDN reflector list used by nxdn_gateway.
# Format: one reflector per line, "talkgroup address port", # starts a comment.
#
# Set NXDN_HOSTS_URL to the list you use.  The default is the Pi-Star list.
URL="${NXDN_HOSTS_URL:-http://www.pistar.uk/downloads/NXDN_Hosts.txt}"
DEST="${1:-/home/pi/OpenMT/protocols/NXDN/NXDNHosts.txt}"

TMP="$(mktemp)"
if curl -fsSL "$URL" -o "$TMP" && grep -q '^[0-9]' "$TMP"; then
    mv "$TMP" "$DEST"
    echo "Updated $DEST"
else
    rm -f "$TMP"
    echo "Download of $URL failed, keeping the old list" >&2
    exit 1
fi
