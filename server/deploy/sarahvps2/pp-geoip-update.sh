#!/bin/sh
# Downloads DB-IP's free "IP to City Lite" database (CC BY 4.0, https://db-ip.com) for mm's ping
# estimate (MM_GEOIP_FILE) and renames it over the old one. mm keeps its mapping of the old file
# and opens the new one within 10 minutes; never write into the live file in place (mm maps it).
# DB-IP publishes a new month on the 1st; early in the month the previous one is taken.
# Runs as ppserver. Installed to /opt/ppserver/libexec/.
set -eu
dir=${PP_GEOIP_DIR:-/var/lib/ppserver/geoip}
out="$dir/dbip-city-lite.mmdb"
tmp="$dir/.dbip-city-lite.mmdb.partial"
mkdir -p "$dir"
trap 'rm -f "$tmp" "$tmp.gz"' EXIT

got=
for month in "$(date -u +%Y-%m)" "$(date -u -d "$(date -u +%Y-%m-01) -1 day" +%Y-%m)"; do
    url="https://download.db-ip.com/free/dbip-city-lite-$month.mmdb.gz"
    if curl -fsS --retry 3 --max-time 600 -o "$tmp.gz" "$url"; then
        got=$month
        break
    fi
done
[ -n "$got" ] || { echo "no DB-IP database could be downloaded" >&2; exit 1; }

gunzip -c "$tmp.gz" >"$tmp"
size=$(stat -c %s "$tmp")
# A real one is over 100 MB and ends with MaxMind's metadata marker.
[ "$size" -gt 50000000 ] || { echo "downloaded database is only $size bytes" >&2; exit 1; }
tail -c 200000 "$tmp" | grep -aq 'MaxMind.com' || { echo "downloaded file is not an .mmdb" >&2; exit 1; }
chmod 0644 "$tmp"
mv "$tmp" "$out"
echo "GeoIP ok: $out ($got, $size bytes)"
