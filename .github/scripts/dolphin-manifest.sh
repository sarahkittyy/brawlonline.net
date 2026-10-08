#!/usr/bin/env bash
# Finishes a Dolphin bundle for the launcher: removes docs (no .md ships), then writes
# dolphin.json, which the launcher compares with the copy it installed under <userData>/netplay.
#
#   .github/scripts/dolphin-manifest.sh <bundle dir> <version> <executable, relative>
set -euo pipefail
dir="${1:?dir}"; version="${2:?version}"; exe="${3:?executable}"
[ -f "$dir/$exe" ] || { echo "$dir/$exe is missing" >&2; exit 1; }
find "$dir" -type f \( -iname '*.md' -o -iname '*.markdown' \) -print -delete
DIR="$dir" V="$version" EXE="$exe" node -e '
  const fs = require("fs"), path = require("path");
  const dir = process.env.DIR;
  let files = 0, size = 0;
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p); else { files++; size += fs.statSync(p).size; } } };
  walk(dir);
  const entries = fs.readdirSync(dir).filter((n) => n !== "dolphin.json").sort();
  const manifest = { version: process.env.V, executable: process.env.EXE, entries, files, size };
  fs.writeFileSync(path.join(dir, "dolphin.json"), JSON.stringify(manifest, null, 2) + "\n");
  console.log(JSON.stringify(manifest));'
