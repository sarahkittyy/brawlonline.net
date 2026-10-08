#!/usr/bin/env bash
# Packages the launcher on macOS: the .app (electron-builder dir target), signed, then the DMG,
# the updater zip and latest-mac.yml from that signed app.
#
#   .github/scripts/package-launcher-macos.sh <out dir> [arm64|x64]
#
# Run after `npm ci`, `npm run stage:plugin` and `npm run build` in launcher/, with the Dolphin
# bundle in launcher/release/dolphin (build-dolphin-macos.sh, signed with the same identity).
#
# Release builds (MAC_SIGN_IDENTITY: the Developer ID Application identity, a name or SHA-1 hash,
# its keychain in the search list; MAC_SIGN_KEYCHAIN optional): the app is signed inside out by
# @electron/osx-sign (hardened runtime, secure timestamp, the launcher's entitlements; Dolphin.app
# keeps its own signature), notarized and stapled; the DMG is signed and notarized, not stapled
# (stapling would change the file after latest-mac.yml recorded its sha512). Notarization uses an
# App Store Connect API key: APPLE_API_KEY_PATH (the .p8), APPLE_API_KEY_ID, APPLE_API_ISSUER.
#
# Without MAC_SIGN_IDENTITY (local test builds) the app is ad-hoc signed: Apple silicon runs it,
# Gatekeeper asks the user to allow it once, and it can't update itself (Squirrel.Mac requires the
# new version's signature to match the running one's). CI never publishes such a build.
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
# Unsigned here: signed below, once app-update.yml is in place.
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

identity="${MAC_SIGN_IDENTITY:--}"
ents=assets/entitlements.mac.plist
# Submits a zip or DMG to Apple's notary service and waits (at most 40 minutes); fails with the
# notary log unless Apple accepts it.
notarize() {
  local file="$1" result id status
  result="$(xcrun notarytool submit "$file" --key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER" --wait --timeout 40m --output-format json)" || true
  echo "$result"
  id="$(node -e 'try { console.log(JSON.parse(process.argv[1]).id || "") } catch { console.log("") }' "$result")"
  status="$(node -e 'try { console.log(JSON.parse(process.argv[1]).status || "") } catch { console.log("") }' "$result")"
  if [ "$status" != "Accepted" ]; then
    echo "notarization of $(basename "$file") failed: ${status:-no result}" >&2
    [ -z "$id" ] || xcrun notarytool log "$id" --key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" \
      --issuer "$APPLE_API_ISSUER" >&2 || true
    return 1
  fi
}

if [ "$identity" = "-" ]; then
  # Ad-hoc signature over the whole app. Dolphin.app inside Resources is already signed with its
  # own entitlements; --deep would re-sign it without them, so the launcher's nested code is signed
  # explicitly (frameworks and helper apps), then the app itself.
  find "$app/Contents/Frameworks" -maxdepth 1 \( -name "*.framework" -o -name "*.app" \) -print0 \
    | while IFS= read -r -d '' nested; do codesign --force --sign - --entitlements "$ents" "$nested"; done
  find "$app/Contents/Resources/app.asar.unpacked" -name "*.node" -print0 2>/dev/null \
    | while IFS= read -r -d '' node; do codesign --force --sign - "$node"; done
  codesign --force --sign - --entitlements "$ents" "$app"
  codesign --verify --deep --strict "$app"
  sign_dmg=(-c.mac.identity=null)
else
  for v in APPLE_API_KEY_PATH APPLE_API_KEY_ID APPLE_API_ISSUER; do
    [ -n "${!v:-}" ] || { echo "$v is not set: a Developer ID build must be notarized" >&2; exit 1; }
  done
  [ -f "$APPLE_API_KEY_PATH" ] || { echo "no API key at APPLE_API_KEY_PATH" >&2; exit 1; }
  # Dolphin.app must already carry this identity's signature (build-dolphin-macos.sh).
  case "$(codesign -dv "$launcher/release/dolphin/Dolphin.app" 2>&1)" in
    *"Authority=Developer ID Application:"*) ;;
    *) echo "release/dolphin/Dolphin.app is not Developer ID signed" >&2; exit 1 ;;
  esac
  # Every Mach-O file inside out (frameworks, helpers, native modules), then the app. Dolphin.app
  # is skipped: it keeps its own signature and entitlements.
  APP="$app" ENTS="$ents" node - <<'JS'
const path = require("path");
const { signAsync } = require("@electron/osx-sign");
const app = path.resolve(process.env.APP);
const dolphin = path.join(app, "Contents", "Resources", "dolphin");
const entitlements = path.resolve(process.env.ENTS);
signAsync({
  app,
  identity: process.env.MAC_SIGN_IDENTITY,
  keychain: process.env.MAC_SIGN_KEYCHAIN || undefined,
  platform: "darwin",
  type: "distribution",
  preAutoEntitlements: false,
  ignore: (file) => file === dolphin || file.startsWith(dolphin + path.sep),
  optionsForFile: () => ({ hardenedRuntime: true, entitlements }),
}).then(
  () => console.log(`signed ${app}`),
  (err) => { console.error(err); process.exit(1); },
);
JS
  codesign --verify --deep --strict "$app"
  codesign -dv "$app" 2>&1 | grep -E '^(Authority|TeamIdentifier|Timestamp)' || true

  tmp="$(mktemp -d)"
  ditto -c -k --keepParent "$app" "$tmp/app.zip"
  notarize "$tmp/app.zip"
  rm -rf "$tmp"
  xcrun stapler staple "$app"
  xcrun stapler validate "$app"
  spctl --assess --type execute -vv "$app"
  # electron-builder signs the DMG (same identity, found by hash or name) before latest-mac.yml
  # records it; the app inside is already signed and is not touched again.
  sign_dmg=(-c.mac.identity="$identity" -c.dmg.sign=true)
fi

# DMG, zip and latest-mac.yml from the signed app.
npx electron-builder build --mac dmg zip "--$arch" --publish never --prepackaged "$app" \
  "${sign_dmg[@]}" -c.mac.hardenedRuntime=false -c.mac.notarize=false
if [ "$identity" != "-" ]; then
  for dmg in release/build/*.dmg; do
    codesign --verify --strict "$dmg"
    case "$(codesign -dv "$dmg" 2>&1)" in
      *"Timestamp="*) ;;
      *) echo "$dmg has no secure timestamp" >&2; exit 1 ;;
    esac
    notarize "$dmg"
  done
fi
mkdir -p "$out"
mv release/build/latest-mac.yml release/build/*.dmg release/build/*.zip "$out/"
mv release/build/*.blockmap "$out/" 2>/dev/null || true
ls -l "$out"
