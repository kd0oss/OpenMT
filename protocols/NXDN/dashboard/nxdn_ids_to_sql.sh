#!/bin/bash
# Created by Rick KD0OSS
# Turns a list of NXDN ids into SQL for modem_host.nxdn_ids.  Input lines are
#   id,callsign,name...   or   id<TAB>callsign<TAB>name...      (# starts a comment)
#   bash nxdn_ids_to_sql.sh NXDN.csv | mysql -u root -p modem_host
awk -F'[,\t]' '
/^#/ { next }
$1 !~ /^[0-9]+$/ || $2 == "" { next }
{
    name = $3
    for (i = 4; i <= NF; i++) name = name " " $i
    gsub(/\047/, "", name); gsub(/\047/, "", $2)
    printf "DELETE FROM modem_host.nxdn_ids WHERE NXDN_Id = %s;\n", $1
    printf "INSERT INTO modem_host.nxdn_ids SET NXDN_Id = %s, Callsign = \047%s\047, Name = \047%s\047;\n", $1, substr($2,1,10), substr(name,1,40)
}' "$1"
