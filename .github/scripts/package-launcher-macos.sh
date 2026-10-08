#!/usr/bin/env bash
# Packages the launcher on macOS: the .app (electron-builder dir target), ad-hoc signed with
# codesign, then the DMG, the updater zip and latest-mac.yml from that signed app.
#
#   .github/scripts/package-launcher-macos.sh <out dir> [arm64|x64]
#
# Run after `npm ci`, `npm run stage:plugin` and `npm run build` in launcher/, with the Dolphin
# bundle in launcher/release/dolphin (build-dolphin-macos.sh).
#
# Without an Apple Developer ID the app is ad-hoc signed: Apple silicon runs it, Gatekeeper asks
# the user to allow it once (System Settings > Privacy & Security > Open Anyway), and the launcher
# offers the DMG instead of updating itself (MAC_SELF_UPDATE in launcher/src/common/product.ts).
set -euo pipefail
out="${1:?out dir}"
arch="${2:-arm64}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
launcher="$repo/launcher"
product="Brawl Online"
case "$arch" in
  arm64) dir_name=mac-arm64 ;;
  x64) dir_name=mac ;;
  *) echo "unknown arch $arch" >&2; exit 2 ;;
esac
[ -f "$launcher/release/dolphin/Dolphin.app/Contents/MacOS/Dolphin" ] \
  || { echo "launcher/release/dolphin has no macOS Dolphin" >&2; exit 1; }

cd "$launcher"
export CSC_IDENTITY_AUTO_DISCOVERY=false
rm -rf "release/build"
npx electron-builder build --mac dir "--$arch" --publish never \
  -c.mac.identity=null -c.mac.hardenedRuntime=false -c.mac.notarize=false
app="release/build/$dir_name/$product.app"
[ -d "$app" ] || { echo "electron-builder made no $app" >&2; exit 1; }

# The updater's config: electron-builder doesn't write it for the dir target. Same content as the
# Linux and Windows packages get (the launcher also sets the feed URL itself).
yml="$app/Contents/Resources/app-update.yml"
if [ ! -f "$yml" ]; then
  name="$(node -p 'require("./release/app/package.json").name')"
  cat >"$yml" <<EOF
provider: generic
url: https://brawlonline.net/updates/launcher
updaterCacheDirName: $name-updater
EOF
fi

# Ad-hoc signature over the whole app. Dolphin.app inside Resources is already signed with its own
# entitlements; --deep would re-sign it without them, so the launcher's nested code is signed
# explicitly (frameworks and helper apps), then the app itself.
ents=assets/entitlements.mac.plist
find "$app/Contents/Frameworks" -maxdepth 1 \( -name "*.framework" -o -name "*.app" \) -print0 \
  | while IFS= read -r -d '' nested; do codesign --force --sign - --entitlements "$ents" "$nested"; done
find "$app/Contents/Resources/app.asar.unpacked" -name "*.node" -print0 2>/dev/null \
  | while IFS= read -r -d '' node; do codesign --force --sign - "$node"; done
codesign --force --sign - --entitlements "$ents" "$app"
codesign --verify --deep --strict "$app"

# DMG, zip and latest-mac.yml from the signed app (electron-builder doesn't sign again: identity null).
npx electron-builder build --mac dmg zip "--$arch" --publish never --prepackaged "$app" \
  -c.mac.identity=null -c.mac.hardenedRuntime=false -c.mac.notarize=false
mkdir -p "$out"
mv release/build/latest-mac.yml release/build/*.dmg release/build/*.zip "$out/"
mv release/build/*.blockmap "$out/" 2>/dev/null || true
ls -l "$out"
