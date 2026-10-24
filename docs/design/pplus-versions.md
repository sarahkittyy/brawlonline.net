# Replays from older Project+ releases (2026-10-20)

A replay only plays the way the game happened on the Project+ release it was recorded on. P+ v3.2.1
changed the codeset, 87 fighter files, stages and sy_core, so a v3.2.0 game run on v3.2.1's card is
refused at its barrier ("the setup differs") or plays differently (seen: a v3.2.0 Room game whose
checksums differed from frame 948, in plain playback too). The launcher moved every player's card
to v3.2.1 (pplus_card_update.ts), so almost every existing replay is from v3.2.0.

The user's decisions (2026-10-20): a small diff hosted by us (unlisted on brawlonline.net), ask
before every download (a "Don't ask again" option was dropped on the user's review the same day), keep the replay card and make it removable, work whether or not
the player has v3.2.0 on their PC, and be future-proof for the next P+ release. Whether the plugin
supports a given release is the one part that can't be future-proof.

## 1. Which release a replay is from

`common/pplus_releases.ts` `replayPPlusRelease`:
- INFO `build.pplus` when the recording has it. The launcher writes `[Online] PPlusRelease` into
  the netplay Dolphin.ini with its other settings, and `ReplayRecorder.cpp` copies it into INFO.
- Otherwise the launcher build that recorded it: `build.dolphin` is `v0.1.<CI run>`, and runs 89
  and lower are v3.2.0, 90 and higher v3.2.1.
- A development build (a commit hash there) is unknown, and plays on the launcher's release as
  before.

The indexer stores it (`game.pplus_release`). The replay database's user version went to 1, so
every file is read again once. The renderer gets it in `GameInfo.pplusRelease`.

## 2. Hosted data (`https://brawlonline.net/updates/pplus/`)

`tools/pplus_packs/make_packs.py` writes these from the older releases' cards and the launcher's
card update lists:

| File | What |
|---|---|
| `index.json` | `{v: 1, newest, blobs: "blobs/{sha256}.zlib", releases: [{release, manifest, sha256, files, filesFromNewest, bytesFromNewest, downloadFromNewest}]}` |
| `releases/<release>.json` | every file of that release's card (`{path, sha256, size}`) and its probe (`/Project+/NETPLAY.GCT`) |
| `blobs/<sha256>.zlib` | a file's bytes, zlib-compressed, named by the sha256 of the bytes: every file an older release has that the newest one does not |

v3.2.0 from v3.2.1 is 142 files: 68.8 MiB, 49 MiB to download. It was built from the official
v3.2.0 zip's card (sha256 7121a0bc...). v3.2.1's manifest is v3.2.0's plus the 134 written and 12
removed files of `pplus_card_update_v3.2.1.json`.

Publishing is `sudo pp-release pplus DIR` on sarahvps2. It checks every blob: a complete zlib
stream, with nothing after it, whose bytes have the sha256 it is named by. It never deletes a blob,
because launchers of every version may still need theirs. It writes the manifests next and
`index.json` last. The folder is unlisted, not secret.

## 3. On the player's PC (`<netplay>/pplus-versions`, launcher `install/pplus_versions.ts`)

- **`index.json`, `releases/*.json`**: the hosted lists, kept for offline use. The index is
  fetched again when a release is prepared.
- **`blobs/<sha256>`**: the files, each checked against its sha256. They come from one of three
  places:
  - the download;
  - a card on this PC that is on that release. "I have Project+ 3.2.0 on this PC" takes a
    Project+ folder or its sd.raw, and every needed file must be there with the right sha256;
  - the card update itself. Before an update writes, the files it overwrites or deletes are kept
    (`keepReplacedFiles`), so the release before the next P+ update needs no download at all.
- **`cards/<release>/sd.raw` + `manifest.json`**: the playback card, made by
  `installPluginOnSdCard` with a `variant`:
  1. a copy of the player's card (2 GB, about 2 s on an SSD);
  2. the release's files written and the current release's extra files deleted (FAT checked, the
     probe read back);
  3. the plugin.

  It is made again when the player's card, the plugin or the release's files change. The player's
  own files (costumes, P+'s replays) stay on it. A card that is not on the launcher's release is
  refused (`card-unknown`).

`PLAYABLE_PPLUS_RELEASES` (v3.2.0, v3.2.1) lists the releases the bundled plugin runs on: the boot
guard scans for BootToCSS's words, and the sy_core stub fix leaves v3.2.0's stubs alone. A replay
of a release outside it is not offered. The player sees "this version of Brawl Online can't play
Project+ X replays yet", with "Play on <current> anyway".

## 4. Playing

**Main.** Every play request goes through main's `decideReplayRelease`: Launch replay, the replay
list's Play, highlights and highlights' Play all. It returns one of:
- `playing`: on the replays' release when its files are here, or on the launcher's own;
- `needs-files`: with the download size;
- `unplayable`.

A request mixing releases plays the first replay's release only, and the renderer says how many
from the other release were left out. The playback Dolphin runs on one card at a time. A request
for another release closes it and starts it again on that release's card (about 11 s, the same as
a cold start). Within a release nothing changes, so highlights stay instant.

**Renderer** (`lib/pplus`). `withReplayVersion` asks with `ReplayVersionDialog` whenever a release's files are
missing (the user's wording, 2026-10-20: "You can download a small patch to have access to replays
from v3.2.0.").
It shows the download (in its compressed bytes) and the card being made. On a failure it says why
(offline, damaged, not that release's card) and offers Try again, Play on <current> anyway, and
Cancel. `ensureReplayRelease(need, "render")` does the same for rendering (later): its wording
talks about clips and rendering, and it offers no "anyway".

**Where players see it:**
- Settings > Replays > Older Project+ Versions lists the releases with their size, Download and
  Remove.
- Replay rows show "Project+ v3.2.0" for a replay from another release.
- The stats page says whether the replay plays on that release's files, or that playing it gets
  them first ("Get it now").

In the launcher's test mode, `window.__brawlTest.replayVersionDialog(view)` shows a state of the
dialog (for screenshots).

## 5. The next P+ release

1. Add a card update as for v3.2.1 (`pplus_card_update_<release>.json`). The update keeps the old
   release's files on every player's PC by itself.
2. Run `make_packs.py --card v3.2.0=<its card> --card v3.2.1=<its card> --update <v3.2.1 list>
   --update <new list> --out DIR`, then `pp-release pplus DIR`. This publishes the blobs for every
   older release, counted from the newest.
3. Once the plugin runs on the new release, add it to `PLAYABLE_PPLUS_RELEASES`. If the plugin
   stops supporting an old release, drop that one: its replays then say so instead of playing wrong.

## Not done

- **Rendering itself.** The dialog's render wording and `ensureReplayRelease` are ready for it. A
  combo movie mixing releases renders its clips per release (one Dolphin per card) and joins them.
- **A different Code Menu setting.** A replay whose recorder had a different P+ Code Menu setting
  is still refused at its barrier ("code menu value 4"). After Play anyway it plays without
  checksums differing. This is older than the version work and does not depend on releases.
- **Development builds.** Their replays (a commit hash in `build.dolphin`) have no release.
