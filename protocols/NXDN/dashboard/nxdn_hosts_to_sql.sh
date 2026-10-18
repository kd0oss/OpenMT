#!/bin/sh
# Turns an NXDNHosts.txt ("talkgroup address port" per line) into SQL for the
# dashboard reflector list. Usage:
#   ./nxdn_hosts_to_sql.sh NXDNHosts.txt | mysql -u root -p modem_host
awk '
/^#/ || NF < 3 { next }
$1 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ { next }
{
    printf "DELETE FROM modem_host.reflectors WHERE refl_type = '\''NXDN'\'' AND Nick = '\''%s'\'';\n", $1
    printf "INSERT INTO modem_host.reflectors SET Nick = '\''%s'\'', ip4 = '\''%s'\'', port = %s, refl_type = '\''NXDN'\'';\n", $1, $2, $3
}' "$1"
