#!/bin/sh
# Created by Rick KD0OSS
# Downloads the YSF reflector list used by ysf_gateway (JSON, as G4KLX's YSFHostsUpdate.sh).
# Set YSF_HOSTS_URL to the list you use.
URL="${YSF_HOSTS_URL:-https://hostfiles.refcheck.radio/YSFHosts.json}"
DEST="${1:-/home/pi/OpenMT/protocols/YSF/YSFHosts.json}"

TMP="$(mktemp)"
if curl -fsSL "$URL" -o "$TMP" && grep -q '"reflectors"' "$TMP"; then
    mv "$TMP" "$DEST"
    echo "Updated $DEST"
else
    rm -f "$TMP"
    echo "Download of $URL failed, keeping the old list" >&2
    exit 1
fi
