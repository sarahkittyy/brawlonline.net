# Brawl Mods: security of the machine image

A player who joins a running Brawl Mods session loads a **machine image** another player made: the whole emulated Wii, Dolphin's savestate of it, and the session NAND's game files (`docs/design/brawl-mods.md` §6.7). It is the first place in this project where data from another player's machine is loaded into the emulator itself, so it is treated like any network input: hostile until checked (§7 of the design). This document is the audit: what the loader reads from the image, what each part could do if crafted, and what has been done about it.

Status (2026-10-12): **first pass of the audit done and its fixes built (§1-§3); the IOS devices, DSP ucodes and `LoadFst` are the second pass; 2 h of fuzzing without a crash (§4), no sanitizer build yet.** Joins stay behind the test switch (`"join": true` in `PPR_MACHINE_SESSION`) until the exit criteria at the end are met.

## 0. The other players vouch for the image (built)

The image's sender controls the joiner's whole emulated machine, not only Dolphin's savestate parser: guest code that hunts for any emulator bug, a crafted device state, a crafted NAND. The defence that does not depend on finding every bug: **at the epoch's frame every player's machine is the same, so every player can compute the image's digest itself.** Each player builds the image's parts at K (the savestate in image mode, the NAND's game files, the inputs of K-1) and sends the joiner their SHA-256 (`epoch_at`, `"digest"`). The joiner waits for every player's digest and loads the image only if its own digest of what it received equals every one of them; it computes it before parsing anything in the image. "Every player" is every connected peer that plays; another joiner (its hello said it does not play) has no digest to give and counts only if it sends one (2026-10-14: two joiners waiting for each other's digest held a REX room at the epoch until one left). A peer that lies about not playing only takes its own vote away; the sender always vouches. A sender who changes one byte of the machine is outvoted by any honest player.

- Measured: the players' digests agree (Brawl Minus at the main menu, 2 players; their whole savestates were already bit-identical at an epoch, `docs/design/brawl-mods.md` §6.7), and the joiner's check passes.
- Left out of the digest: the map of GPU-written granules (host-dependent; a crafted one can only make the joiner's checksums disagree, a desync report).
- **What it does not cover:** a room of two, where the only player is the sender (the host whose game it is), and a room where every other player colludes. For those the hardening below and the policy of the design's §7 step 5 apply.

## 1. The image envelope (built, checked)

| What | Check |
|---|---|
| Header (channel 2, 36 bytes) | magic, epoch, frame, checksum, packed and raw sizes; packed at most 256 MiB, raw at most 768 MiB; only from the peer that the epoch's request named, only while joining |
| Chunks | appended only up to the announced packed size; anything past it drops the image |
| Decompression | zstd frame content size must equal the announced raw size and the limit; window at most 128 MiB (`ZSTD_d_windowLogMax` 27) |
| Layout | every length (state, GPU map, NAND) is checked against the bytes left; the GPU maps must be exactly MEM1/64 and MEM2/64 bytes; the frame and memory sizes must match the joiner's |
| NAND files (`UnpackNand`) | only `tmp/`, `title/`, `shared2/` and `fst.bin`; printable ASCII; no empty, `.` or `..` component; no `\` or `:`; at most 4096 entries; each size within the image. Written below the session's temporary NAND root only |
| After loading | the joiner's RAM checksum must equal the sender's, or the join fails |

## 2. Dolphin's savestate loader

### 2.1 `PointerWrap` (Common/ChunkFile.h), used by every `DoState`

| Primitive | Read-mode behaviour | Risk |
|---|---|---|
| `Do(T)` / `DoArray` / `DoVoid` (trivially copyable) | bounds-checked: past the end the wrap switches to measure mode and stops copying; the load then fails (`State::LoadFromBuffer` returns false) | none found |
| `Do(std::vector/string/deque)`, `DoEachElement` | **resizes the container to a count read from the stream before checking the bytes exist** | a count of 2^32-1 allocates gigabytes: `std::bad_alloc` (Dolphin terminates) or memory exhaustion. Denial of service |
| `Do(std::map)`, `Do(std::set)`, `Do(std::list)` | loop `count` times; after the wrap fails they keep looping, doing nothing | a count of 2^32-1: a multi-second hang. Denial of service |
| `DoPointer(x, base)` | `x = base + offset`, offset from the stream, **unchecked** | an arbitrary pointer. Used by the FIFO (below): a later write through it is an arbitrary memory write in Dolphin: **code execution** |
| `DoExternal(count)` | moves past `count` bytes, fails the wrap if past the end, returns the old position; callers must check the mode before using it | the two callers do (FS, texture cache) |

### 2.2 Findings by area (first pass)

| Area | What the stream decides | Risk | Plan |
|---|---|---|---|
| `FifoManager::DoState` | the video buffer's read and write pointers (`DoPointer`) | **High**: arbitrary write | **fixed**: bounded to `[0, FIFO_SIZE]` |
| `SerialInterface::DoState` | each port's device type; a different type is created | **High**: types such as the GBA link open host sockets | **fixed** (image mode): a different type fails the load |
| `ExpansionInterface` channels | each slot's device type, created if different | **High**: the Ethernet adapters open network connections, memory cards host files | **fixed** (image mode) |
| `Wiimote::DoState` | each Wii Remote's source; "real" reconnects a real one | Medium | **fixed** (image mode) |
| `TextureCacheBase`, `FramebufferManager`, `BoundingBox` | texture configs (size, levels, layers, format) handed to the GPU driver, EFB contents | **High** surface (GPU drivers), no value: host-only, not part of the game's state | **fixed**: not in images |
| `AchievementManager::DoState` | RetroAchievements progress, parsed by rcheevos; a buffer sized from the state | Medium | **fixed**: not in any session state |
| `DSPHLE::DoState` | the HLE ucode, chosen by CRC; then that ucode's `DoState` | Medium: limited to Dolphin's built-in ucodes (AX, AXWii, Zelda, CARD, INIT, ROM, ASnd, AESnd) | audit each; consider pinning to the ucode the session's game uses |
| IOS: `Kernel::DoState`, devices, ES, FS | device names, fds (a USB passthrough device for type OH0), IPC request addresses, ES title context (TMD/ticket bytes parsed by `Formats.cpp`), socket poll commands | Medium-High | **partly fixed**: fds only emulated devices in images, unknown fd types fail, title context validated, poll command count bounded; per-device pass next |
| `HostFileSystem` (`FS.cpp`), `fst.bin` | handle paths and offsets (paths are validated by `BuildFilename`), the FST tree from `fst.bin` (`LoadFst`: depth-limited) | Medium | audit `LoadFst` and the handle table |
| `CoreTiming` | the event queue; event types are looked up by name (unknown: a no-op "lost" event) | Low | check queue size limit (each event costs at least a few bytes, so §2.1's count fix bounds it) |
| `Memory` | MEM1, MEM2, L1 (fixed sizes, checked against the joiner's) | Low: guest memory | none |
| `PowerPC` | registers, MSR, BATs, TLB; BAT changes remap fastmem | Low-Medium: guest-visible state, but the mapping code must cope with any BAT value | audit `DBATUpdated`/`IBATUpdated` with arbitrary BATs |
| `Gecko::DoState` | active Gecko codes | Low (they write guest memory only) | image mode: no codes (cheats are off in a session) |

## 3. Hardening (built 2026-10-12 unless marked)

1. **`PointerWrap`** (`Common/ChunkFile.h`): `CheckReadCount` fails the load before a container (vector, string, deque, list, map, set) is sized from a count the bytes left cannot hold (each element takes at least one byte; a trivially copyable one its size); map and set loops stop once the load failed; `DoPointer` takes a bound and rejects an offset outside it. Manual loaders use `CheckReadCount` too (`WiiSockMan::DoState`).
2. **Image mode** (`Machine::InImageDoState`, set on both sides around the image's save and load): the host-only video state (texture cache, framebuffer contents, bounding box) stays out (the joiner rebuilds it from RAM; the image went from 185 to 121 MB raw); a different SI or EXI device type, Wii Remote source, or a descriptor that is not one of the emulated devices (USB passthrough) fails the load. RetroAchievements' state is never in a whole-machine session's states.
3. **Fixes found on the way** (any savestate, any disc): an XF transfer that starts before the post-transform matrices or the lights and reaches into them marked its change from their *base address* instead of offset 0, and the next draw copied the matrices to `posttransformmatrices[320]` / `lights[96]` of 64- and 8-entry arrays (**a write far past the shader constants**, from guest commands; upstream Dolphin's `XFStateManager::InvalidateXFRange`; found when the joiner marked all of XF changed after loading an image and crashed); the FIFO's pointers are bounded to the FIFO; a loaded ES title context must hold a valid TMD and ticket (their getters assume one); the AX and AX Wii command list parsers stop at the buffer's end (a list from guest memory without an end command read past it; 32 words of padding cover a command's own reads). The emulated Wii Remote's active extension (an index into its attachments, called through: **a pointer read past the list's end**) and report mode (an invalid one left no report builder) are checked as a game's requests are; the Bluetooth stack's queued events and ACL packets may not claim more bytes than their buffers (their size is copied into guest memory: a larger one leaked host memory).
4. **No host network for the guest** (2026-10-13): a session wants determinism (`Core::WantsDeterminism`), which upstream already made the IP stack's ioctls and SSL refuse, but not the vector ioctls: `GETADDRINFO` sent the guest's names to the host's DNS (a build could make every player's machine look up any name), `SENDTO`/`RECVFROM` reached the socket manager, and `GETINTERFACEOPT` gave the guest the host's address. Those refuse too now, and so do WiiConnect24's network requests (register, check, download, send) and its scheduler, which ran on the host's clock. Found as a desync (Brawl's Wi-Fi login asked the host's DNS on a worker thread, `docs/design/brawl-mods.md` §6.6), but it closes a leak as well.
5. **Second pass (to do):** each IOS device's state (DI, STM, SDIO, USB hosts, the network devices), the Wii Remote's extensions, camera, speaker and Motion Plus, the DSP ucodes other than AX (and a null ucode after an unknown CRC), `HostFileSystem::LoadFst` with a crafted `fst.bin`, `PowerPC` with arbitrary BATs, `CoreTiming`'s event types.
6. A failed load ends the join with "Could not load the session's machine: …"; the players go on without the joiner. (The joiner's own machine is not restored: it leaves the session.)

## 4. Testing

- **The fuzzer** (`harness/tools/fuzz_machine_image.py`, built): boots a game in a harness Dolphin, saves its state as a machine image carries it, and loads mutated copies with the image's rules (byte flips, 32-bit fields set to edge values, truncation, chunks copied within the image; 90 % outside guest RAM, which is what the loader parses); after each accepted one the machine runs half a second and the pristine state must load back. A Dolphin that dies leaves its crash report and the mutation; `--only N` replays one.
- **Results** (release build, no sanitizer, Brawl Minus at its menus):
  - 2026-10-12, 2 h: 8,237 mutated images, 7,021 loaded and ran, 1,216 refused, **0 crashes**, the pristine state loaded back every time.
  - An earlier 2 h run (before the fuzzer took a fresh pristine state after restarting a Dolphin) reported two failures; both replayed cleanly (`--only 802`: loads and runs; `--only 1085`: refused). Neither left a crash report: the harness connection had dropped, not the Dolphin.
  - One crash in an earlier, shorter run did not reproduce: a mutation of the FIFO's unprocessed commands (guest-reachable: the guest writes those commands itself).
- **Next:** an AddressSanitizer build (MSVC `/fsanitize=address`) for the same fuzzer, and mutations aimed at the second pass's areas (§3.4).
- **Exit criteria** (design §7): no crash and no sanitizer report over 24 hours of fuzzing. Until then images are accepted only in private rooms from friends (design §10, question 4).
