# Social: players online, direct messages, invites (launcher)

_2026-10-10. The launcher's player list, its end-to-end encrypted DMs and its game invites. Server: `server/crates/accounts/src/social.rs`. Launcher: `launcher/src/social/` (main process: connection, keys, state), `launcher/src/renderer/components/social/` and `pages/home/overview/online_players/` (UI). The in-game chat it shares keys and the ignore list with is `docs/chat-protocol.md`._

## 1. What the player sees

- **Online list** under the rating on Home: everyone whose launcher is open and logged in (with online play set up), in a random order (each player gets a random place in your list when they come online and keeps it until they leave, so a name starting with "_" or "1" doesn't put anyone at the top), each name followed by its connect code in a lighter grey and an envelope invite button on the right. Hovering a row highlights it like a public room's row. A left click opens a chat tab; a right click opens the player's menu: **Message**, **Invite to game**, **Ignore user** / **Unignore user**.
- **Chat tabs** along the bottom of the window (a 32 px bar the pages leave room for), from the bottom left to the right, shrinking to fit. A tab's header holds the name, the tag, the invite button (right-aligned) and a close button; clicking it opens or closes the chat window, which draws over the page above it. A tab with unread messages pulses amber. The same right-click menu works on a tab's header. In the box, Enter sends and keeps the focus; Ctrl+Backspace and Ctrl+W delete the word before the caret; Esc closes the window.
- **Invites** pop up in the bottom right: "bob invited you to their room. Join?" (or "to a Direct 1v1") with **Yes** (green), **No** (grey) and an X (dismiss without answering). Shown for 110 s (the server keeps an invite answerable for 120 s).
- **Sounds** (the player's own disc, `src/brawl_assets/catalog.ts`): a message plays Fox's shine unless its chat window is open and the launcher has focus; an invite plays the star KO twinkle. The bell left of the settings gear turns both off ("Toggle Notification Sounds", setting `notificationSounds`).
- **Ignore**: an ignored player's messages are kept but shown as dark spoiler boxes (a click shows one), open no tab, mark nothing unread and play no sound; their invites are dropped. The list is the game's (`<User>/Online/chat-hidden.json`), so ignoring in the launcher ignores in game and the other way round.

## 2. Keys

The launcher uses the install's chat identity key, `<User>/Online/chat-identity.key` in the netplay Dolphin's User folder (`docs/chat-protocol.md` §1): whoever needs it first creates it (a temporary file hard-linked into place, so nobody reads half a file), the other reads it. Each stream makes a fresh X25519 **session key** `kx` and signs it with the identity key under the group id `dm-<own uid>`:

`kxSig = Ed25519.sign(identity, "brawlonline-chat-kx-v1" ‖ 0 ‖ "dm-<uid>" ‖ 0 ‖ kx)`

A conversation between uids `a` and `b` is the group `dm-<lo>-<hi>` (the two uids in byte order, 76 characters). Pair keys, message signatures, plaintexts, associated data and boxes are exactly `docs/chat-protocol.md` §4's with that group id, so the same code and test vectors cover both. `seq` starts at 1 per conversation and stream; a receiver keeps the highest `seq` per sender and sender session key and drops anything not above it.

## 3. The stream

`GET /v1/social/stream`, session auth (`Authorization: Bearer`), headers `X-Chat-Key` (identity key, 64 hex), `X-Chat-Kx` (64 hex), `X-Chat-Kx-Sig` (128 hex). Refused with 403 `online_required` before online play is set up (a verified email and a connect code, not banned), 400 `invalid_keys` for a malformed or weak identity key or a `kxSig` that doesn't verify, 429 after 10 connects a minute or 60 an hour, 503 `social_full` beyond 5000 launchers. accounts records the identity key in `chat_identity_keys`, like mm does for games.

The answer is server-sent events: one JSON object per `data:` line, a `: ping` comment every 15 s (nginx's 30 s read timeout and Cloudflare's 100 s pass idle streams; `X-Accel-Buffering: no` makes nginx pass events on at once). A launcher that hears nothing for 45 s reconnects. Each launcher has a queue of 256 events; one that stops reading is dropped (its stream ends) and everyone gets its `offline`. One stream per account: a new one replaces the old, which gets `replaced` and ends.

| Event | Fields |
|---|---|
| `welcome` | `you` (uid), `online` (members, by name, the receiver left out), `room` (the receiver's room or null). Always first. |
| `online` | `member`: a player came online, or came back with a new session key. |
| `offline` | `uid` |
| `dm` | `from`, `kx` (the sender's session key the box was sealed with), `box` (hex) |
| `invite` | `from`, `kind`: `room` with `room`, or `direct` with `connectCode` (the inviter's) |
| `invite-answer` | `by`, `kind`, `accept`, `connectCode` (the invitee's) |
| `room` | `room`: the receiver's room changed (joined, left, slots, status) or null |
| `replaced` | another launcher took over the account |
| `account` | an admin renamed the account (`pp-admin user rename`): read `/v1/me` again; the others got its `online` again with the new name |
| `banned` | the account was banned (`pp-admin user ban`): the stream ends and its session is gone; the others get its `offline` |

A member is `{uid, displayName, connectCode, idKey, kx, kxSig}`. A room is `{code, players, openSlots, mode, status, joinable}` (`status` `waiting`, `starting` or `in-game`; `joinable`: an open slot is empty).

Rooms live in mm. mm's status snapshot (`GET /status` on its local listener) carries `members`, every room player's room by uid, public and private rooms alike. accounts reads it at most once a second, only while launchers are connected, to send `room` events and to check invites; `GET /v1/rooms` never includes it.

## 4. Requests

All session auth, JSON bodies.

| Request | Answer | Refusals |
|---|---|---|
| `POST /v1/social/dm {to, box}` | 204 | 409 `not_connected` (no stream), 404 `offline`, 400 `invalid` (a box outside 114..512 bytes, not hex, a bad uid, oneself), 429 over 5 in 5 s or 30 a minute |
| `POST /v1/social/invite {to}` | 200 `{kind}` | 409 `not_connected`, 404 `offline`, 409 `room_full` (in a room with no empty open slot), 409 `already_in_room`, 429 over one to the same player per 5 s, 10 a minute or 60 an hour, 503 `mm_unavailable` |
| `POST /v1/social/invite/answer {from, accept}` | 204 | 404 `no_invite` (none pending from that player, or older than 120 s), 409 `not_connected` |

An invite from a player in a room is to that room (`kind: room`); from a player in none, to a **Direct 1v1** (`kind: direct`).

## 5. Joining

The launcher hands the game a code the way clicking a public room does (`docs/rooms-protocol.md` §5, `join-room.json`): a running game gets it at once, else the launcher leaves it for the game and starts it like Play.

- **Room invite, accepted:** the invitee's game joins the room by its code. The inviter is told ("bob is joining your room.").
- **Direct invite, accepted:** both games get a Direct request (`{"kind": "direct", "connectCode": …}`) naming the other player's code: the invitee's from the invite, the inviter's from `invite-answer`. Each goes to Direct 1v1's character select with the code filled in and searches it on START, so mm pairs them as an ordinary Direct match.
- **No / declined:** the inviter gets a toast. The X answers nothing.

## 6. Hostile input

Everything from the server is checked before it is used: uids by pattern, names through the chat cleaner (32 code points), connect codes by pattern, keys by length, rooms field by field, at most 5000 members, 64 KiB per event (4 MiB for `welcome`). A DM is used only if it comes from a listed player with the session key that player was listed with, whose `kxSig` verifies against their identity key (checked on first use), opens with the pair key, has a `seq` above the last one from that key and a valid signature, and its text survives the cleaner (`docs/chat-protocol.md` §5). Invites must come from a listed player; a Direct invite's code must be that player's own. The renderer only ever gets cleaned text and draws it as text.

Nothing is stored on disk except the identity key and the ignore list: conversations last as long as the launcher runs.
