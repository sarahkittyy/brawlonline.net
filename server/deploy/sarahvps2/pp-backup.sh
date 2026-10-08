#!/bin/sh
# pg_dump of the `pp` database (custom format), checked with pg_restore --list,
# then dumps older than PP_BACKUP_KEEP_DAYS are deleted. Runs as postgres
# (peer auth over the local socket). Installed to /opt/ppserver/libexec/.
set -eu
dir=${PP_BACKUP_DIR:-/var/lib/ppserver/backups}
keep=${PP_BACKUP_KEEP_DAYS:-14}
stamp=$(date -u +%Y%m%dT%H%M%SZ)
tmp="$dir/.pp-$stamp.dump.partial"
out="$dir/pp-$stamp.dump"

pg_dump --format=custom --dbname=pp --file="$tmp"
pg_restore --list "$tmp" >/dev/null
mv "$tmp" "$out"

# -mtime +N means "more than N whole days old", so this keeps the last $keep days.
find "$dir" -maxdepth 1 -name 'pp-*.dump' -mtime +"$((keep - 1))" -print -delete
find "$dir" -maxdepth 1 -name '.pp-*.partial' -mmin +60 -delete
echo "backup ok: $out ($(stat -c %s "$out") bytes), $(find "$dir" -maxdepth 1 -name 'pp-*.dump' | wc -l) dumps kept"
