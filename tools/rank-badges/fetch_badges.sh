#!/usr/bin/env bash
# Puts the rank badge PNGs (make_badges.py's output) where the launcher and the website build them in:
#
#   launcher/src/renderer/components/rank_badge/images/<tier>-<division>.png        256 px
#   launcher/src/renderer/components/rank_badge/images/small/<tier>-<division>.png  128 px
#   website/assets/ranks/<tier>-<division>.png                                      128 px
#
# They are not in git. The emblems are Layerlab's 2D League Icon Pack: its license allows using them in
# the launcher and on the website, but not publishing the files, and this repository is public
# (docs/design/ranked-tiers.md §5.1). The production server keeps them as a tarball that only answers
# with the token: CI passes the RANK_BADGES_TOKEN secret.
#
#   RANK_BADGES_TOKEN=... bash tools/rank-badges/fetch_badges.sh
#
# Without a token (a fork, a fresh clone) it writes a transparent placeholder for each missing badge,
# so the launcher and the website still build; the rank's name still shows on hover. In CI a missing
# token is an error, so a release never ships placeholders. Files already there are kept, unless they
# are placeholders and a token is given.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
URL="${RANK_BADGES_URL:-https://brawlonline.net/build-assets/rank-badges}"
DIRS=(
  launcher/src/renderer/components/rank_badge/images
  launcher/src/renderer/components/rank_badge/images/small
  website/assets/ranks
)
NAMES=(legendary)
for tier in bronze silver gold emerald diamond master; do
  for div in 1 2 3; do NAMES+=("$tier-$div"); done
done
MARK=.placeholders  # in a folder whose badges are placeholders

missing() {
  for d in "${DIRS[@]}"; do
    [ -e "$ROOT/$d/$MARK" ] && { echo "$d/$MARK"; continue; }
    for n in "${NAMES[@]}"; do [ -f "$ROOT/$d/$n.png" ] || echo "$d/$n.png"; done
  done
}

if [ -z "$(missing)" ]; then
  echo "rank badges: all ${#NAMES[@]} x ${#DIRS[@]} in place"
  exit 0
fi

if [ -n "${RANK_BADGES_TOKEN:-}" ]; then
  tmp="$(mktemp)"
  trap 'rm -f "$tmp"' EXIT
  curl -fsS --retry 3 --max-time 60 -H "Authorization: Bearer $RANK_BADGES_TOKEN" -o "$tmp" "$URL" ||
    { echo "rank badges: download failed ($URL)" >&2; exit 1; }
  for d in "${DIRS[@]}"; do rm -f "$ROOT/$d/$MARK"; done
  tar -xzf "$tmp" -C "$ROOT"
  left="$(missing)"
  [ -z "$left" ] || { echo "rank badges: the tarball lacks:" >&2; echo "$left" >&2; exit 1; }
  echo "rank badges: fetched ${#NAMES[@]} x ${#DIRS[@]} from $URL"
  exit 0
fi

if [ -n "${CI:-}" ]; then
  echo "rank badges: RANK_BADGES_TOKEN is not set; a release must not ship placeholder badges" >&2
  exit 1
fi

# A 1x1 transparent PNG.
PNG="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNgYGBgAAAABQABeqhXUAAAAABJRU5ErkJggg=="
for d in "${DIRS[@]}"; do
  mkdir -p "$ROOT/$d"
  wrote=""
  for n in "${NAMES[@]}"; do
    f="$ROOT/$d/$n.png"
    [ -f "$f" ] && continue
    printf '%s' "$PNG" | base64 -d > "$f"
    wrote=1
  done
  [ -z "$wrote" ] || touch "$ROOT/$d/$MARK"
done
echo "rank badges: no RANK_BADGES_TOKEN, so transparent placeholders (see tools/rank-badges/fetch_badges.sh)"
