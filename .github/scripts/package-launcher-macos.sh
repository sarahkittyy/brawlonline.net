#!/usr/bin/env bash
# Packages the launcher for macOS on Linux: the .app from electron-builder (dir target, no signing),
# ad-hoc signed with rcodesign, then the updater zip, a DMG and latest-mac.yml.
#
#   .github/scripts/package-launcher-macos.sh <version> <out dir> [arm64|x64]
#
# Run from anywhere after `npm ci`, `npm run stage:plugin` and `npm run build` in launcher/, with
# the Dolphin bundle in launcher/release/dolphin (build-dolphin-macos.sh). Needs rcodesign,
# genisoimage and libdmg-hfsplus's `dmg` on PATH (docs/ci.md "macOS").
#
# Without an Apple Developer ID the app is ad-hoc signed: Apple silicon runs it, Gatekeeper asks
# the user to allow it once (System Settings > Privacy & Security > Open Anyway), and macOS can't
# auto-update it in place (Squirrel.Mac checks the new version's signature against the old one).
set -euo pipefail
version="${1:?version}"
out="${2:?out dir}"
arch="${3:-arm64}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
launcher="$repo/launcher"
product="Brawl Online"
case "$arch" in
  arm64) eb_arch=arm64; dir_name=mac-arm64 ;;
  x64) eb_arch=x64; dir_name=mac ;;
  *) echo "unknown arch $arch" >&2; exit 2 ;;
esac
[ -f "$launcher/release/dolphin/Dolphin.app/Contents/MacOS/Dolphin" ] \
  || { echo "launcher/release/dolphin has no macOS Dolphin" >&2; exit 1; }

cd "$launcher"
electron_version="$(node -p 'require("./node_modules/electron/package.json").version')"

# better-sqlite3 is native: fetch its prebuilt binary for Electron on macOS (it can't be compiled
# here), and let electron-builder skip its own rebuild for the target platform.
(
  cd release/app/node_modules/better-sqlite3
  rm -rf build prebuilds
  npx --yes prebuild-install@7 --runtime electron --target "$electron_version" \
    --platform darwin --arch "$eb_arch" --tag-prefix v --verbose
  file build/Release/better_sqlite3.node | grep -q "Mach-O" \
    || { echo "better-sqlite3: no macOS binary" >&2; exit 1; }
)

rm -rf "release/build/$dir_name"
CSC_IDENTITY_AUTO_DISCOVERY=false nice -n 19 npx electron-builder build --mac dir "--$eb_arch" \
  --publish never \
  -c.npmRebuild=false -c.mac.identity=null -c.mac.hardenedRuntime=false -c.mac.notarize=false
app="release/build/$dir_name/$product.app"
[ -d "$app" ] || { echo "electron-builder made no $app" >&2; exit 1; }

# Linux better-sqlite3 again for anything after this in the same checkout (tests, Linux package).
(cd release/app/node_modules/better-sqlite3 && rm -rf build && npx --yes prebuild-install@7 \
  --runtime electron --target "$electron_version" --platform linux --arch x64 --tag-prefix v) || true

# ---- Sign (ad-hoc). Nested code first: rcodesign walks the bundle (frameworks, helper apps,
# Dolphin.app under Resources is already signed and is sealed as a resource).
rcodesign sign --entitlements-xml-file assets/entitlements.mac.plist "$app" >"$out.sign.log" 2>&1 \
  || { tail -n 40 "$out.sign.log"; exit 1; }
grep -q "signing" "$out.sign.log" || { echo "rcodesign signed nothing" >&2; exit 1; }

mkdir -p "$out"
zip_name="Brawl-Online-$version-$arch-mac.zip"
dmg_name="Brawl-Online-$version-$arch.dmg"
rm -f "$out/$zip_name" "$out/$dmg_name"

# Updater zip: symlinks kept (Electron's frameworks are full of them).
(cd "release/build/$dir_name" && zip -qry --symlinks "$out/$zip_name" "$product.app")

# DMG: an HFS hybrid image (genisoimage) with the app and an Applications link, compressed with
# libdmg-hfsplus into a UDZO image macOS mounts like any other DMG.
stage="$(mktemp -d)"
cp -a "$app" "$stage/"
ln -s /Applications "$stage/Applications"
genisoimage -quiet -no-cache-inodes -D -l -probe -no-pad -r -dir-mode 0755 -apple \
  -V "$product" -o "$stage.hfs" "$stage"
dmg "$stage.hfs" "$out/$dmg_name" >/dev/null
rm -rf "$stage" "$stage.hfs"

# latest-mac.yml in electron-builder's format (sha512 base64, the zip first: MacUpdater uses it).
sha() { openssl dgst -sha512 -binary "$1" | base64 -w0; }
size() { stat -c %s "$1"; }
zsha="$(sha "$out/$zip_name")"
dsha="$(sha "$out/$dmg_name")"
cat >"$out/latest-mac.yml" <<EOF
version: $version
files:
  - url: $zip_name
    sha512: $zsha
    size: $(size "$out/$zip_name")
  - url: $dmg_name
    sha512: $dsha
    size: $(size "$out/$dmg_name")
path: $zip_name
sha512: $zsha
releaseDate: '$(date -u +%Y-%m-%dT%H:%M:%S.000Z)'
EOF
ls -l "$out"
