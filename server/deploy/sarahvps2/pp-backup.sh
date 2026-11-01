#!/bin/sh
# pg_dump of the `pp` database (custom format), checked with pg_restore --list,
# then dumps older than PP_BACKUP_KEEP_DAYS are deleted. Runs as postgres
# (peer auth over the local socket). Installed to /opt/ppserver/libexec/.
#
# sd_card_listings is dumped without its rows (Sarah's board, 2026-11-01: the
# backups grew by about a gig a day). Its listings are a cache of hosts' SD
# cards, about 2 MB each of chunk hashes that do not compress: 570 of a
# 600 MB dump, 30 MB without them. Hosts' launchers put a card's listing back
# the first time they host it after a restore; until then a joiner is asked
# about the card without its sizes.
set -eu
dir=${PP_BACKUP_DIR:-/var/lib/ppserver/backups}
keep=${PP_BACKUP_KEEP_DAYS:-14}
stamp=$(date -u +%Y%m%dT%H%M%SZ)
tmp="$dir/.pp-$stamp.dump.partial"
out="$dir/pp-$stamp.dump"

pg_dump --format=custom --dbname=pp --exclude-table-data=sd_card_listings --file="$tmp"
pg_restore --list "$tmp" >/dev/null
mv "$tmp" "$out"

# -mtime +N means "more than N whole days old", so this keeps the last $keep days.
find "$dir" -maxdepth 1 -name 'pp-*.dump' -mtime +"$((keep - 1))" -print -delete
find "$dir" -maxdepth 1 -name '.pp-*.partial' -mmin +60 -delete
echo "backup ok: $out ($(stat -c %s "$out") bytes), $(find "$dir" -maxdepth 1 -name 'pp-*.dump' | wc -l) dumps kept"
