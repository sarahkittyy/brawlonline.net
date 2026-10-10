# Chat: free-text chat with the other players (design)

_2026-10-10. The user's decisions from two question rounds; the wire format is `docs/chat-protocol.md`. Replaces the options draft `chat-ideas.md` (its option B, with the transport changed: rooms keep no peer-to-peer link between games)._

## 1. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Where | **Dolphin draws it with ImGui** (its OSD layer), over the game. Not the launcher, not the game's own UI. |
| 2 | Who | **Everyone you are playing with**: a room's members, or the other player of a Direct, Unranked or Ranked match. On for everyone by default, strangers included. |
| 3 | Transport | **mm relays end-to-end encrypted messages** over each game's online connection (`hello`, `docs/rooms-protocol.md` §2). Rooms have no peer-to-peer link between games, so the same path serves matches too. mm never sees the text. |
| 4 | Group crypto | **Pairwise, one copy per recipient**: each player makes a fresh X25519 key pair per room or match; each pair of players derives its own key; a message is encrypted once per other member (at most 3 copies). Joins and kicks need no rekeying. |
| 5 | Signatures | **Each install has an Ed25519 identity key** (registered with the server through `hello`). Every message and every group key is signed inside the encryption, so a report carries proof the server can check, and reports can't be made up. The server still can't read chat. |
| 6 | Library | **Monocypher** 4 (one C file, audited, public domain/BSD) with its optional Ed25519 file, in Dolphin's Externals. The server and `mmclient` use the RustCrypto crates for the same primitives. |
| 7 | During a match | **Shown by default; players can type mid-match.** A saved option, "Show during matches", hides the window for every match without clicking each time; messages that arrive meanwhile are there when the match ends. |
| 8 | Placement | **In the side bar** when the game is 4:3 in a wider window, **else the top-left** of the game. The window is **draggable, resizable and collapsible**, and remembers where it was put. |
| 9 | Typing | The rebindable hotkey **"Activate NetPlay Chat" (default T)** opens the box. While it has focus, **only keyboard and mouse input is blocked** from the game; GameCube adapters and controllers keep playing. |
| 10 | Hide | **Click a name: Hide messages / Show messages / Report.** A hidden player's lines become a dark box the size of the text (Discord's spoiler); clicking one box reveals that message only. Kept by account (not by name), across matches and rooms. |
| 11 | Font | **Noto Sans + Noto Sans JP** (SIL OFL), shipped in `Sys/Resources`, so kana and kanji in names and messages render on every platform. |

## 2. Threat model

| Who | Can | Can't |
|---|---|---|
| Someone on the network path | See that chat happens, its size and timing | Read it, change it, inject messages (all are authenticated) |
| Another player | Send text (cleaned, size- and rate-limited), be hidden or reported | Crash or corrupt the game (every byte is checked), spoof another player's name (names come from mm), replay old messages, fake a report about someone else |
| A compromised mm | Hand out its own keys and read what is sent afterwards, drop or delay messages | Read past chat, forge a message that verifies against an existing identity key |

The play key travels in `hello` unencrypted, like Slippi's tickets; that is out of this design's scope (noted for later).

## 3. What the receiver does with hostile input

In this order, each step dropping the message if it fails (counted per reason):

1. Size first: packets over mm's 8 KiB are refused by both sides; JSON is checked for depth and number overflow (`Gprb::PeerData::JsonSafeToParse`) before picojson sees it.
2. The group is the current one; the sender is a current member whose group key verified against their identity key.
3. Hex of bounded length, decrypt and authenticate (XChaCha20-Poly1305, key bound to the group and both key pairs, sender and recipient in the associated data).
4. Version byte, sequence number higher than any seen from that sender in this group (no replays), signature valid.
5. The text cleaner (`docs/chat-protocol.md` §5): valid UTF-8; control, bidi, zero-width, tag, variation-selector and private-use characters removed; whitespace runs collapsed; at most 2 combining marks in a row; at most 100 code points and 300 bytes.
6. Per-sender rate limit (on top of mm's), then the line goes into a history of at most 100.

Drawing uses `ImGui::TextUnformatted` only (never a printf-style call with peer text), and logs pass peer text only as `fmt` arguments. Nothing is ever interpreted: no markup, no links. Fonts have a fixed set of glyphs, so no text can grow the font atlas past them.

## 4. Lifetime

- **Rooms:** one group per room, for as long as the player is in it. Joins and leaves show as grey lines.
- **Matches:** one group per match, from the moment mm pairs the players. Dolphin shows it once the peer-to-peer session is up (a failed connect requeues and the next ticket ends the group). It ends when either player searches again, joins a room, leaves the online CSS, or quits.
- A new group starts with an empty history. Nothing is stored on disk except the hidden list and the identity key.

## 5. Not in scope

- Quick chat (Slippi's D-pad messages): still a candidate for controller-only players.
- Chat in the launcher, friends lists, DMs.
- Moderation UI: reports are stored and listed by `admin chat-reports`.
