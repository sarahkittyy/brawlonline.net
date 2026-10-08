#!/bin/sh
# Replace dolphin/ with a snapshot of a branch from a local Dolphin checkout, in one commit.
#
#   tools/sync-dolphin.sh [SRC] [BRANCH]
#     SRC     local Dolphin repo (default: ../dolphin next to this repo)
#     BRANCH  branch or commit to snapshot (default: rollback-fixes)
#   NO_COMMIT=1   stage only
#
# Stopgap until Dolphin development moves into this repo. What it does:
#   - copies the branch's tree (exact blobs, no line-ending changes) into dolphin/;
#   - keeps Dolphin's submodules: dolphin/Externals/... gitlinks at the pinned commits, and
#     .gitmodules entries rewritten to those paths with the same upstream URLs;
#   - drops game data (Wii title/save data, disc/SD images, .dol/.rel binaries; see EXCLUDE);
#   - writes dolphin/NOTICE-BRAWLONLINE.txt with the source and upstream base commits.
# Only committed content of BRANCH is used. Untracked files in dolphin/ (build dirs,
# initialized submodules) are left alone.
set -eu

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
SRC="${1:-$ROOT/../dolphin}"
BRANCH="${2:-rollback-fixes}"
PREFIX=dolphin
# Brawlback's Project-Plus-Dolphin branches whose merge-base is recorded in the NOTICE.
UPSTREAM_REFS="${UPSTREAM_REFS:-origin/rollback origin/master}"
# Paths (extended regex on the path inside Dolphin's tree) that are never imported.
EXCLUDE='^Data/Sys/NetplaySave/|^Data/user/Wii/|(^|/)title/0001000[0-9a-fA-F]/|\.(iso|raw|dol|rel|pac|brres|brstm|wbfs|rvz|gcz|ciso|wad|gct)$'

git -C "$SRC" rev-parse --git-dir >/dev/null
COMMIT="$(git -C "$SRC" rev-parse --verify "$BRANCH^{commit}")"
TREE="$(git -C "$SRC" rev-parse "$COMMIT^{tree}")"

if ! git diff --quiet HEAD -- "$PREFIX" .gitmodules || ! git diff --cached --quiet -- "$PREFIX" .gitmodules; then
  echo "error: $PREFIX/ or .gitmodules has uncommitted changes" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 1. Copy the tree's objects (no history) into this repo.
if ! git cat-file -e "$TREE" 2>/dev/null; then
  echo "copying objects of $BRANCH ($COMMIT) ..."
  git -C "$SRC" rev-list --objects "$TREE" | git -C "$SRC" pack-objects --stdout -q | git index-pack --stdin >/dev/null
fi

# 2. Replace the index entries under dolphin/.
git -c core.quotepath=off ls-files -- "$PREFIX" | sort > "$TMP/old"
git rm -r -q --cached --ignore-unmatch -- "$PREFIX" >/dev/null
git read-tree --prefix="$PREFIX/" "$TREE"
git -c core.quotepath=off ls-files -s -- "$PREFIX" | cut -f2- | sed "s#^$PREFIX/##" | grep -E "$EXCLUDE" > "$TMP/excluded" || true
if [ -s "$TMP/excluded" ]; then
  sed "s#^#$PREFIX/#" "$TMP/excluded" | tr '\n' '\0' | git rm -q --cached --pathspec-from-file=- --pathspec-file-nul >/dev/null
fi
git -c core.quotepath=off ls-files -- "$PREFIX" | sort > "$TMP/new"

# 3. Update the working tree: delete files that left, write the rest from the index.
comm -23 "$TMP/old" "$TMP/new" | while IFS= read -r f; do
  if [ -f "$f" ] || [ -L "$f" ]; then rm -f -- "$f"; fi
done
git ls-files -s -- "$PREFIX" | awk '$1 != "160000"' | cut -f2- | tr '\n' '\0' |
  git checkout-index -f -z --stdin
# Uninitialized submodules are empty directories.
git ls-files -s -- "$PREFIX" | awk '$1 == "160000"' | cut -f2- | while IFS= read -r p; do mkdir -p -- "$p"; done

# 4. Submodules: dolphin's .gitmodules, with paths under dolphin/.
touch .gitmodules
git config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | while read -r key path; do
  case "$path" in "$PREFIX"/*)
    name="${key#submodule.}"; name="${name%.path}"
    git config -f .gitmodules --remove-section "submodule.$name" ;;
  esac
done
if git -C "$SRC" cat-file -e "$COMMIT:.gitmodules" 2>/dev/null; then
  git -C "$SRC" show "$COMMIT:.gitmodules" > "$TMP/gitmodules"
  git config -f "$TMP/gitmodules" --get-regexp '^submodule\..*\.path$' | while read -r key path; do
    name="${key#submodule.}"; name="${name%.path}"
    new="$PREFIX/$path"
    if [ "$(git ls-files -s -- "$new" | cut -c1-6)" != "160000" ]; then
      echo "warning: .gitmodules lists $path but the tree has no gitlink there; skipped" >&2
      continue
    fi
    git config -f .gitmodules "submodule.$new.path" "$new"
    git config -f .gitmodules "submodule.$new.url" "$(git config -f "$TMP/gitmodules" "submodule.$name.url")"
    for opt in branch shallow update ignore; do
      v="$(git config -f "$TMP/gitmodules" "submodule.$name.$opt" || true)"
      if [ -n "$v" ]; then git config -f .gitmodules "submodule.$new.$opt" "$v"; fi
    done
  done
fi
git ls-files -s -- "$PREFIX" | awk '$1 == "160000" {print $4}' | while read -r p; do
  git config -f .gitmodules "submodule.$p.path" >/dev/null || echo "warning: gitlink $p has no .gitmodules entry" >&2
done
git add .gitmodules

# 5. NOTICE with the source and the upstream bases.
SUBJECT="$(git -C "$SRC" log -1 --format=%s "$COMMIT")"
URL="$(git -C "$SRC" remote get-url origin 2>/dev/null || echo unknown)"
BASES=""
for r in $UPSTREAM_REFS; do
  if tip="$(git -C "$SRC" rev-parse -q --verify "$r^{commit}")"; then
    mb="$(git -C "$SRC" merge-base "$COMMIT" "$tip")"
    BASES="$BASES  $r: merge-base $mb (\"$(git -C "$SRC" log -1 --format=%s "$mb")\"; that ref is at $tip)
"
  fi
done
NEXCL="$(wc -l < "$TMP/excluded" | tr -d ' ')"
cat > "$PREFIX/NOTICE-BRAWLONLINE.txt" <<EOF
Brawl Online: Dolphin snapshot
==============================

dolphin/ is a snapshot (no history) of our Dolphin fork, branch $BRANCH at
  $COMMIT ("$SUBJECT")

Our fork is based on Brawlback's Project-Plus-Dolphin ($URL),
which is based on Dolphin (https://github.com/dolphin-emu/dolphin). Upstream base commits:
$BASES
The full history of all three lives in those repositories.

License: Dolphin is GPL-2.0-or-later (see COPYING and LICENSES/); our changes are under the
same license. Third-party code under Externals/ keeps its own licenses (Externals/licenses.md).

Submodules under Externals/ are pinned to the same upstream commits as in our branch;
run "git submodule update --init --recursive" after cloning.

Not imported ($NEXCL files): game data that Project-Plus-Dolphin keeps in its tree, i.e. the
Brawl save template (Data/Sys/NetplaySave/, Data/user/Wii/title/) and the Project+ launcher
.dol binaries (Data/user/Launcher/*.dol). The launcher takes these from the user's own
Project+ files.
EOF
git add -- "$PREFIX/NOTICE-BRAWLONLINE.txt"

if [ -n "${NO_COMMIT:-}" ]; then
  echo "staged dolphin/ from $BRANCH @ $COMMIT (not committed)"
  exit 0
fi
if git diff --cached --quiet; then
  echo "dolphin/ already matches $BRANCH @ $COMMIT"
  exit 0
fi
{
  echo "dolphin: snapshot of $BRANCH @ ${COMMIT%"${COMMIT#??????????}"}"
  echo
  echo "Source: $BRANCH $COMMIT (\"$SUBJECT\")"
  echo "Upstream bases:"
  printf '%s' "$BASES"
  echo
  echo "Excluded $NEXCL game-data files; see dolphin/NOTICE-BRAWLONLINE.txt."
} > "$TMP/msg"
git commit -q -F "$TMP/msg"
git log -1 --format='committed %h %s'
