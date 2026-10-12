# Brawl Mods: whole-game rollback for any build (design)

_2026-10-12. Branch `brawl-mods`. The user's decisions from the conversation of 2026-10-12, plus defaults chosen without asking (marked **default**: say if one is wrong). Nothing here changes the P+ online modes (Direct, Unranked, Ranked, rooms): they keep their in-game menus and gameplay-only rollback._

---

## 1. What it is

A second way to play online, for every build of Brawl that is not the P+ release we ship, and for other GameCube and Wii games where it works:

- **The whole game is rolled back, from boot.** Every player runs the same machine. The menus are shared: everyone sees the same main menu, character select and stage select, and anyone's controller can drive them, as when friends sit at one console. Nothing in the game is ours, so nothing in the game has to be ported per mod.
- **Plug in and out like a controller at a LAN.** A friend who joins gets the running game and their controller is plugged into the next port at an agreed frame. A player who leaves is unplugged the same way. Everyone else keeps playing.
- **The launcher holds the room.** The game shows no room UI. The launcher shows who is in the room, the slots, public or private, kick, and the build everyone must have.

Why: Brawl Minus, REX, vanilla Brawl and other mods have no online play with rollback. Our P+ menus are the best way to play P+, but they cannot follow every mod (tested on Brawl Minus 5.1, 2026-10-12: the plugin loads, the WITH FRIENDS page crashes, and the P+-only state it reads does not exist). Whole-game rollback needs no knowledge of the game.

## 2. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Where | A **"Brawl Mods" tab next to Overview** on the launcher's home page, with a short explanation. Overview stays the default and the launcher stays P+-first. This is new UI that Slippi does not have; the user asked for it (2026-10-12). |
| 2 | Which games | **Any build of Brawl** first (vanilla Rev 1 and Rev 2, Brawl Minus, REX, P+ itself). **Any GameCube or Wii game** that Dolphin's own netplay keeps in sync, as a later step. Games that desync on Dolphin netplay are expected to desync here too; players understand that (user, 2026-10-12). |
| 3 | Identity of a build | **A build hash** replaces the ruleset: the disc, the launcher `.dol` (if any) and the SD card's contents. A table of known hashes names the common builds ("Brawl Minus 5.1.0b", "Vanilla Brawl (Rev 1)"); anything else is "Unknown build" with its short hash. Hover shows the full hash; the tab shows the player's own builds and their hashes. Only players with the same hash can join. |
| 4 | Rev 1 and Rev 2 | **Allowed together if testing shows it works** (**default**). The two discs differ in one instruction (the DVD ID check, `0x8001BC9C`) and the bss word it fills; whole-game rollback keeps each machine's own copy and the desync check skips that word. Not allowed together until tested. |
| 5 | Session | **Whole-machine rollback from boot**, menus included, built on our Dolphin's whole-machine snapshots (dirty-granule saves, dual core with the split video state: `docs/rollback-fixes-status.md`). |
| 6 | Players | **1 to 4**. A room's host plays at once; others plug in and out at any time (decision 8). |
| 7 | Controllers | **GameCube controllers and Wii Remotes**, with Nunchuk, Classic Controller and MotionPlus (user, 2026-10-12). Each player's own controller config and profile, as in Dolphin's netplay. |
| 8 | Joining | **The joiner gets the running machine from a peer** (RAM, device state, NAND), loads it, catches up on the inputs since, and is plugged in at an agreed frame. Nobody else stops. Replaying every input since boot was rejected as the main path: its cost grows with the session (Orca: ~600 frames/s on an M4 Pro, so an hour of play takes ~6 minutes to join). **Security must be evaluated and confirmed before this ships** (user, 2026-10-12): §7. |
| 9 | Leaving | **Unplug at an agreed frame**; the others play on. The port's controller is removed the way the game sees a real unplug. |
| 10 | Rooms | **Public or private, set in the launcher.** The game has no room UI. A **room window** (a launcher pop-up) shows the players, slots, kick, slot changes and public/private. |
| 11 | Chat | **The existing end-to-end encrypted chat overlay** over the Dolphin window (`docs/chat-protocol.md`), unchanged. |
| 12 | Desyncs | Detected from confirmed frames (§6.6). **A desync is repaired by sending the host's machine to everyone who differs** (the join path), with a message; the session goes on (**default**). |
| 13 | Ranked | **None.** Mod rooms are never rated or recorded as ranked games (**default**). |
| 14 | Dual core | **Required**, as for P+ (project constraint). The whole-machine path already runs dual core with the deterministic GPU thread. |

## 3. How it works, end to end

```
 Launcher (Brawl Mods tab)                Dolphin (one per player)                   mm (rooms, signaling)
 ─────────────────────────                ──────────────────────────                 ─────────────────────
 pick a build, Host ──launch──────────▶   boot the build, solo session  ──hello────▶ room (kind "mods", build hash)
 room window ◀──local control channel──▶  room client, chat overlay      ◀─room-state─
                                          P2P ENet host (session)       ──session ticket (from P2P port)──▶
 friend: Join KFQB ──launch──────────▶    boot nothing yet; join room  ◀─peers' endpoints───
                                          ◀═══════ P2P: machine image, inputs, GekkoNet ═══════▶
                                          plugged in at frame K; play
```

- **Dolphin is the room client**, as it is for P+ rooms today (`Online/Rooms.cpp`, one online connection per account). The launcher talks to the running Dolphin through a **local control channel**: a loopback TCP connection with a random token, both written to `<User>/Online/control.json` (**default**; it replaces file polling for anything live). Room clicks from the launcher's public list reach Dolphin the same way (or through Dolphin's command line when it is not running).
- **Host:** the launcher starts Dolphin with the build and `--mods-room=host[,private]`. Dolphin boots the build, starts a session of one, opens the room on mm and reports it on the control channel; the launcher opens the room window.
- **Join:** the launcher starts Dolphin with `--mods-room=join:KFQB` (or tells the running one). Dolphin joins the room (mm checks the build hash), registers its P2P endpoint, receives the members' endpoints, connects P2P, asks the session host for the machine, loads it, catches up, and is plugged in.

## 4. The build

A build is what the player picks once in the Brawl Mods tab ("Add a build"):

| Part | What | Hash |
|---|---|---|
| Disc | The game's disc image (Brawl: `RSBE01`) | The game ID, revision and Dolphin's disc hash (the same hash the P+ disc check uses) |
| Launcher | Optional `.dol` booted instead of the disc (Minus Launcher, P+ launchers). Riivolution is a later option | SHA-256 of the file |
| SD card | Optional: an image (`sd.raw`) or a folder Dolphin syncs into an image | SHA-256 over the card's **files** (path, size, contents, sorted), not the raw image, so a card rebuilt from the same files hashes the same |
| Settings that change what the game computes | Dolphin's emulation settings a session pins anyway (§6.8) | not part of the hash: the session forces them |

- **Build hash** = SHA-256 of the three parts above; shown as 8 hex digits (`3f9a1c07`), full on hover.
- **Known builds** come from a small table in the launcher (`launcher/src/mods/known_builds.ts`): hash → name, version, homepage. The table only names; unknown builds play the same way.
- **The SD card is read-only during a session.** Writes are refused (the game sees a write-protected card) (**default**; Nyx's fork does the same). Saves on the NAND work (§6.5).
- Hashing a 2 GB image file by file takes a few seconds; the launcher caches the result by (path, size, mtime).

## 5. Rooms (server)

Mod rooms are mm rooms with a different kind. What changes from `docs/rooms-protocol.md`:

| | P+ rooms | Mod rooms |
|---|---|---|
| Create | `room-create {public, ruleset}` | `room-create {kind: "mods", build: {hash, name}, public}` |
| Join | anyone | only with the same `build.hash` (`room-error` "You need <name> (<hash>) to join.") |
| Ruleset, rules, stages, sets, ready, teams | yes | **none** (refused) |
| Start | when every open slot is ready | **never**: the host's session runs from creation; members plug in |
| Slots | open/close, kick by closing | the same, plus **slot moves** (`room-move {slot}` by a member to an open empty slot; `room-move {from, to}` by the host). A move is an unplug and a plug-in at the same agreed frame. |
| Public list | `GET /v1/rooms` | the same list, with `kind` and `build` per room; the P+ list filters `kind: "pplus"`, the Mods tab `kind: "mods"` |
| Session signaling | room ticket at the start | **`session-endpoint`** (from the P2P port, like a ticket: mm records the public endpoint) and **`session-peers`** pushed to every member on every change (slot, endpoint, connect code, display name) |

mm stays the only server involved. No game data goes through it: the machine image and inputs are P2P. Rate limits as for P+ rooms. Kicked or banned players cannot rejoin the same room for 10 minutes (**default**).

## 6. Dolphin

### 6.1 The frame boundary

Snapshots are taken, and inputs change, at a **frame boundary** that must fall on exactly the same instruction on every machine.

- **Brawl builds:** Brawl's main-loop hook (`0x80017504`, `BrawlbackGekkoNetFrameEnd`), as the whole-machine path does today. Checked clean against P+'s and Brawl Minus's codesets (2026-10-12). Every Brawl build runs this `main.dol`.
- **Other games:** the start of each **VI field**. CoreTiming services an event only at the end of the JIT block that crosses its time, and block layout differs between machines after rollbacks, so the field boundary would land on different instructions. Nyx's fork (Brawlback-Team/Project-Plus-Dolphin, `Dolphin-Wide-Rollback`, GPL) solves it: a guard event 4096 ticks before the field arms the interpreter, which checks the downcount after every instruction, so the VI event is serviced at the same instruction everywhere; the boundary runs after `CoreTiming::Advance` returns, outside generated code. **We port that mechanism with attribution** (`CoreTiming`, `SystemTimers`, `VideoInterface`, the Jit64 and JitArm64 dispatchers, the cached interpreter). The Brawl hook stays the default for Brawl because it is one game frame per boundary (Brawl's logic runs 0-3 steps per field while loading).

### 6.2 Inputs

Inputs enter where the hardware does, so nothing depends on the game:

- **GameCube pads:** `SI_DeviceGCController::GetPadStatus` already asks netplay (`NetPlay_GetInput`). In a session every SI poll of frame F returns frame F's pad for that port.
- **Wii Remotes:** the emulated Bluetooth stack asks netplay once per Wii Remote update (`BTEmu.cpp`, `NetPlay_GetWiimoteData`, 200 Hz) with a `SerializedWiimoteState` (at most 31 bytes: buttons, acceleration, IR points, MotionPlus, extension). In a session every update of frame F uses frame F's state. Dolphin's netplay sends this state per update in lockstep, which stalls the CPU thread (Nyx disabled Wii Remotes for that reason); we latch it per frame instead, so it is predicted and rolled back like a pad.
- **One player, one port.** A player in slot N drives GameCube port N and Wii Remote N. Their input per frame (**default**, 48 bytes):

  | Bytes | Field |
  |---|---|
  | 0 | flags: bit 0 GC pad present, bit 1 Wii Remote connected |
  | 1-8 | GC pad: buttons (2), stick X/Y, C-stick X/Y, L, R |
  | 9 | Wii Remote state length (0-31) |
  | 10-40 | `SerializedWiimoteState::data` |
  | 41-47 | reserved, zero |

- **Plugging** is a change of the flags at an agreed frame: the GC port's SI device is set (`SerialInterface::ChangeDevice`, which takes effect through its own CoreTiming event, the same on every machine) and the Wii Remote is connected or disconnected (`WiimoteDevice::Activate`). The SI device types and Wii Remote sources are pinned for the session; a player's own choice of GC pad or Wii Remote (and which extension) travels in their input.
- **Prediction:** a remote player's last confirmed input repeated (GekkoNet's), Wii Remote included.

### 6.3 Sessions and epochs

- **GekkoNet** carries inputs (fixed size, §6.2), as in the P+ paths. Its player count is fixed per session, so a session is a chain of **epochs**: each change of membership (plug-in, unplug, slot move, kick) ends the epoch at an agreed frame K and starts a new GekkoNet session at K with the new set of players. Frame numbers keep counting across epochs.
- **Agreeing on K:** the session host proposes K = (its newest frame) + margin (**default** 30 frames) on the control channel of the P2P link; every member acknowledges; members that cannot reach K in time (a stalled peer) are dropped first. The host is the room's host; when it leaves, the room's next host takes over (mm already moves the host).
- **Transport:** the P2P ENet hosts and hole punching of `Online/Matchmaking.cpp`, with GekkoNet's adapter over them (as `GameplaySession` does). Channel 0: GekkoNet; channel 1: session control (JSON, small); channel 2: machine images (§6.7), rate-limited.
- **Pacing, time sync, presentation:** reused from the P+ gameplay session where they are not Brawl-specific (`CoreTimingManager::BeginRollbackBurst`, present pacing, audio clock waiting during resimulated passes).

### 6.4 Snapshots

`RollbackManager` in whole-machine mode (not region mode): MEM1, MEM2, the locked L1 cache and Dolphin's device state, saved incrementally (64-byte granules written since the last save). Measured on P+ (2026-10, Fox/Falco, distance 2): **0.90 ms per save, 1.57 ms per load**. The Brawl-specific parts of the path that exists today move behind the session: pads written into Brawl's `gfPadStatus` (replaced by §6.2), Brawl's desync checksums (replaced by §6.6), the Brawl frame counter.

### 6.5 NAND and SD

- **NAND:** the session runs on a temporary NAND, as Dolphin's netplay does. A rollback must also undo file writes: a **write journal** records each file's previous contents (or absence) at its first change after a snapshot; a load undoes the journal back to the snapshot's mark (**default**; Orca and Nyx do this, `IOS/FS/HostBackend/Journal.cpp`, GPL, ported with attribution). Saves made during the session are written back to the player's own NAND at the end only if the user ticks "Keep saves from this session" (**default** off).
- **SD:** read-only (§4).

### 6.6 Desyncs

- Each confirmed frame's checksum (GekkoNet compares them) is **CRC32 of the dirty granules of MEM1 and MEM2 since the last confirmed frame**, skipping granules that the GPU thread wrote (EFB and XFB copies to RAM: Brawl copies 4x4 rendered pixels into RAM for its lighting, which differs between GPUs and backends; `docs/determinism-findings.md`) and the Rev 1 / Rev 2 disc ID word.
- A desync shows "Desync detected, resyncing" in the OSD and the session host sends its machine to the players that differ (§6.7). Twice in a minute ends the session with "Desync: this build does not stay in sync online" (**default**).

### 6.7 Joining: the machine image

1. The joiner's Dolphin boots the same build with nothing run (it never executes the game before the image loads).
2. The session host captures an image at a boundary: MEM1 and MEM2 (from the rollback base snapshot plus the ring, so no pause), the device state (`State::DoState` minus RAM), the NAND files that differ from a fresh session NAND, and the frame number F0.
3. The image is compressed (zstd), checksummed and sent on channel 2 at no more than 2 MB/s (**default**; Orca saw an unthrottled upload delay pings). Nobody stops.
4. The joiner validates and loads it (§7), then runs the frames F0..K from the inputs the host relays, unthrottled and unrendered.
5. At K everyone starts the next epoch with the joiner plugged in.

Expected size: MEM1 24 MiB + MEM2 64 MiB + device state (a few MiB) + NAND; compressed size to be measured (target under 40 MB for Brawl on a menu).

## 7. Security of the machine image

The machine image is the first thing in this project where **one player's machine loads data another player made into the emulator itself**. It must be evaluated and confirmed before release (user, 2026-10-12).

**What an image can do:**

| Part | Risk | Why |
|---|---|---|
| MEM1, MEM2, L1 | **Low** | Guest memory: the emulated console runs it. It can only do what any game disc could do in Dolphin: emulator escapes would be Dolphin bugs reachable by any disc. |
| NAND files | **Medium** | Paths and sizes come from the peer: path traversal (`../`), huge files, file counts. Validated like any peer data: a fixed allowlist of directories (the title's data folder, `shared2`, `sys`), no `..`, total size and count limits. |
| Device state (`DoState`) | **High** | Dolphin's savestate loader trusts its input: counts read from the stream size vectors and index arrays, pointers are rebuilt from offsets, enum values select code paths. A crafted state could corrupt Dolphin's own memory: code execution on the joiner's PC. |

**Plan:**

1. **Audit** every `DoState` reachable from `State::DoState` for the session's configuration (Wii, the pinned devices): list each value read from the stream that sizes an allocation, indexes an array, selects a type, or becomes a pointer or length. Record the list in `docs/brawl-mods-security.md`.
2. **Harden** the load: `PointerWrap` read mode checks every read against the buffer end (it does; verify) and fails the whole load on the first error; every count and index found in step 1 is checked against its real bound; allocations from stream sizes are capped. A failed load leaves the joiner on a fresh boot with "Could not load the game from <host>".
3. **Fuzz** the hardened loader: mutate real images (byte flips, length fields, truncation, splices between images) and load them in a harness Dolphin, with ASan in a dedicated build. Exit criterion: no crash and no sanitizer report in a long run (**default** 24 hours across the configs of step 1).
4. **Limit who sends:** only the session host, which the room's host is (or the next host). The joiner can see who it is getting the game from. In a 3-4 player room the other members' confirmed checksums after the plug-in show whether the image was honest (a lying host desyncs from everyone else).
5. **Fallback:** if step 3 is not met before release, images are only accepted in **private rooms from players in the joiner's friends list** (**default**), and public rooms use input replay from boot (slow but safe) until it is.

## 8. Launcher

- **Brawl Mods tab** (`launcher/src/renderer/pages/home/mods/`): a short explanation; "Your builds" (vanilla detected from the disc set in Settings, plus "Add a build": disc, optional launcher `.dol`, optional SD image or folder; each shows its name or "Unknown build", the short hash and the full hash on hover); per build **Host** (public or private) and **Join** (room code box); the public mod rooms, filtered to builds the player has, others greyed with "You don't have this build".
- **Room window** (a separate `BrowserWindow`, opened while the player is in a mod room): the room code (copy), public/private (host), four slots with name, connect code and controller (GC or Wii Remote), slot move, kick (host), leave; the build; ping per player.
- Theming as the rest of the launcher. Nothing on Overview changes.

## 9. Phases

| Phase | What | Done when |
|---|---|---|
| 1 | Dolphin: machine session of 2 players started from the command line (peer address, seat, build), boot together, Brawl boundary, GC pads | Two harness Dolphins play vanilla Brawl and Brawl Minus from boot through menus into a match under netsim `typical`, 0 desyncs, dual core |
| 2 | Wii Remotes (all extensions); NAND journal; SD read-only; generic desync checksums | The same with one player on Wii Remote + Nunchuk and one on Classic Controller; a save written during the session and rolled back stays consistent |
| 3 | Epochs; 3-4 players; unplug, slot move | 4 harness Dolphins; a player leaves and another moves slot mid-match, the rest play on, 0 desyncs |
| 4 | Machine image join (unhardened, behind a test switch) | A third player joins a running 2-player match and plays; checksums agree after the plug-in |
| 5 | Security: audit, hardening, fuzzing (§7) | §7 step 3's exit criterion; the security doc |
| 6 | mm: mod rooms and signaling; Dolphin room client; local control channel | Host and join through a local mm, two machines on the LAN (this PC and the Mac) |
| 7 | Launcher: tab, builds and hashes, room window, public list | Staging run (`tools/staging/staging.py`) with Windows and Mac clients |
| 8 | VI boundary for non-Brawl games | A non-Brawl game boots and stays in sync (which game: §10) |
| 9 | Desync repair by resend, Rev 1 / Rev 2 interop test, Linux and Steam Deck | — |

## 10. Open questions (defaults in effect until answered)

1. **Which non-Brawl game to test with** (phase 8)? Only Brawl discs are on this PC. Default: phase 8 waits for a disc from the user.
2. **Riivolution builds** (some mods ship as Riivolution XML instead of a launcher `.dol`): later, after phase 7.
3. **Keep saves from the session** (§6.5) off by default.
4. **Who may send an image before §7 is met** (§7 step 5): friends in private rooms.
