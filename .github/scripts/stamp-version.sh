#!/usr/bin/env bash
# Stamps the release version into this CI checkout (never committed):
#   - an annotated tag v<version> at HEAD, so Dolphin's ScmRevGen (git describe) reports
#     "Project+ Dolphin v<version>" and the build counts as a tagged release;
#   - Dolphin's Online::APP_VERSION (matchmaking appVersion, compared with user.json's latestVersion);
#   - the launcher's release/app/package.json version (electron-builder and the update feed).
#
#   .github/scripts/stamp-version.sh 0.1.42
set -euo pipefail
version="${1:?version}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "bad version $version" >&2; exit 1; }
cd "$(dirname "$0")/../.."

git -c user.name=ci -c user.email=ci@brawlonline.net tag -a -f -m "v$version" "v$version" HEAD >/dev/null
echo "git describe: $(git describe --always --long)"

user_h=dolphin/Source/Core/Core/Online/User.h
if [ -f "$user_h" ]; then
  # Not `sed -i`: GNU and BSD (macOS) sed take its argument differently.
  sed -E "s/^(constexpr char APP_VERSION\[\] = )\"[^\"]*\";/\1\"$version\";/" "$user_h" >"$user_h.tmp"
  mv "$user_h.tmp" "$user_h"
  grep -q "APP_VERSION\[\] = \"$version\";" "$user_h" || { echo "could not stamp $user_h" >&2; exit 1; }
  echo "$user_h: APP_VERSION $version"
fi

app_pkg=launcher/release/app/package.json
if [ -f "$app_pkg" ]; then
  V="$version" node -e '
    const fs = require("fs");
    for (const f of process.argv.slice(1)) { // node -e: argv[1] is the first argument
      if (!fs.existsSync(f)) continue;
      const j = JSON.parse(fs.readFileSync(f, "utf8"));
      j.version = process.env.V;
      if (j.packages && j.packages[""]) j.packages[""].version = process.env.V;
      fs.writeFileSync(f, JSON.stringify(j, null, 2) + "\n");
    }' "$app_pkg" launcher/release/app/package-lock.json
  echo "$app_pkg: version $(node -p "require('./$app_pkg').version")"
fi
