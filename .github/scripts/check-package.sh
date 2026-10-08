#!/usr/bin/env bash
# Checks an unpacked launcher package (electron-builder's linux-unpacked / win-unpacked) before it
# is published: no docs (.md) anywhere, including inside app.asar; LICENSE and NOTICE (GPL) and
# the bundled Dolphin and game plugin are present; no game files.
#
#   .github/scripts/check-package.sh <unpacked dir>      (run from launcher/release/build)
set -euo pipefail
dir="${1:?unpacked dir}"
res="$dir/resources"
fail=0
err() { echo "check-package: $*" >&2; fail=1; }

for f in LICENSE NOTICE plugins/PPOnline.rel plugins/PPOnline.json dolphin/dolphin.json app.asar app-update.yml; do
  [ -e "$res/$f" ] || err "missing resources/$f"
done
[ -e "$res/dolphin/COPYING" ] || err "missing the Dolphin license (resources/dolphin/COPYING)"

docs=$(find "$dir" -type f \( -iname '*.md' -o -iname '*.markdown' \) | head -n 20)
[ -z "$docs" ] || err "docs in the package:"$'\n'"$docs"

# Game and P+ data must never ship (the launcher downloads P+ from P+'s release at setup).
game=$(find "$dir" -type f \( -iname '*.iso' -o -iname '*.raw' -o -iname '*.dol' -o -iname '*.rvz' \
  -o -iname '*.wbfs' -o -iname '*.pac' -o -iname '*.brres' -o -iname '*.brstm' \) | head -n 20)
[ -z "$game" ] || err "game files in the package:"$'\n'"$game"
[ ! -e "$res/dolphin/Sys/NetplaySave" ] && [ ! -e "$res/dolphin/usr/bin/Sys/NetplaySave" ] ||
  err "the Brawl save template (Sys/NetplaySave) is in the package"

asar_md=$(node -e '
  const asar = require("@electron/asar");
  const files = asar.listPackage(process.argv[1], { isPack: false });
  for (const f of files) if (/\.(md|markdown)$/i.test(f)) console.log(f);' "$res/app.asar" | head -n 20)
[ -z "$asar_md" ] || err "docs inside app.asar:"$'\n'"$asar_md"

echo "resources: $(ls "$res" | tr '\n' ' ')"
echo "dolphin: $(node -p 'const m=require(process.argv[1]); `${m.version} ${m.executable} ${m.files} files`' "$(cd "$res/dolphin" && pwd)/dolphin.json")"
[ "$fail" -eq 0 ] && echo "check-package: ok"
exit "$fail"
