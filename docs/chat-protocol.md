# Chat: protocol and crypto

_2026-10-10. The decisions behind it are in `docs/design/chat.md`. Server: `server/crates/mm/src/chat.rs`, `server/crates/common/src/chat.rs` (types and crypto, shared with `mmclient`). Dolphin: `Source/Core/Core/Online/Chat*.{h,cpp}`, the window in `Source/Core/VideoCommon/OnlineChatUI.cpp`._

All messages travel on the online connection (`docs/rooms-protocol.md` §2): JSON in reliable ENet packets on channel 0, at most 8 KiB. Binary values are lowercase hex (mm also reads upper case, and sends lowercase). A uid is the account's UUID in its 36-character hyphenated lowercase form.

## 1. Identity key

Each install keeps an Ed25519 key pair (RFC 8032). Dolphin stores the 32-byte seed as 64 hex digits in `<User>/Online/chat-identity.key` (created on first use, mode 0600 on Unix). `hello` carries the public key:

```json
{"type": "hello", "user": {"uid": "…", "playKey": "…"}, "appVersion": "0.1.50", "platform": "win",
 "chatKey": "3d40…(64 hex)"}
```

`chatKey` is optional (older builds); a connection without one can't chat (`chat-*` requests get `chat-error` "Update the game to chat."). A malformed `chatKey` is ignored the same way: not a string, not 64 hex digits, not a valid Ed25519 point, or a small-order ("weak") point. It never makes mm refuse the `hello`. `chat-*` on a connection that sent no `hello` (or on a ticket connection) is answered like a room request there: `{"type": "error", "error": "Log in again in the launcher."}` and a disconnect. mm records each (uid, key) it accepts in `chat_identity_keys` (first and last seen; the 20 most recent per account are kept), which is what reports are checked against.

## 2. Groups

mm puts each online connection in **at most one** chat group:

| Group | Id | Members | Ends for a member when |
|---|---|---|---|
| Room | `room-<CODE>-<n>` (`n` counts rooms since mm started) | The room's players (every occupied slot) whose online connection has a `chatKey` | They leave or are removed from the room, or their connection drops |
| Match | the match's `matchId` (Direct, Unranked, Ranked; never a room's game) | The matched players that have an online connection with a `chatKey` | They send `chat-leave`, a ticket (any mode), `room-create` or `room-join`, or their connection drops |

Entering a group leaves the previous one. A group with no members is forgotten. Ids are at most 96 characters of `A-Z a-z 0-9 . _ : + -`. A room's game (mode-3 tickets) changes nothing in the chat: the room's group goes on. A ticket ends the account's match group only after its play key checked out, and a new `hello` from the same account (another game) takes the old connection out of its group like a drop.

### `chat-group` (server → client)

Sent to every member after every change (a member joins or leaves, a member publishes its group key), and `{"type": "chat-group", "group": null}` (no other fields) to a connection that leaves its group and is still connected. A connection that goes straight into another group (a match made while it was in a room's group) gets only the new group's `chat-group`, which replaces the old one. `room-create` and `room-join` from a match group send the null first, then the room's group:

```json
{"type": "chat-group", "group": "room-KFQB-12", "kind": "room", "you": "<uid>",
 "members": [
   {"uid": "…", "displayName": "alice", "connectCode": "ALIC#4", "idKey": "<64 hex>",
    "kx": "<64 hex>", "kxSig": "<128 hex>"},
   {"uid": "…", "displayName": "bob", "connectCode": "BO#77", "idKey": "<64 hex>",
    "kx": null, "kxSig": null}
 ]}
```

`kind` is `room` or `match`. Members are in joining order, at most 4, the receiver included. `kx` and `kxSig` are null until that member has sent `chat-key` for this group.

### `chat-key` (client → server)

```json
{"type": "chat-key", "group": "room-KFQB-12", "kx": "<64 hex>", "sig": "<128 hex>"}
```

The sender's X25519 public key for this group, made fresh for the group, and its Ed25519 signature (§4). mm checks only that the group is the sender's and the lengths; clients check the signature. A second `chat-key` for the same group replaces the first. `chat-key` and `chat-leave` (of a match group) together are limited to 10 in 10 s per account ("Too many requests. Wait a moment."), since each makes mm push the group to every member.

### `chat-leave` (client → server)

`{"type": "chat-leave", "group": "…"}`: leaves a match group (Dolphin sends it when the online session is cleaned up). Ignored for room groups and for a group the sender is not in.

## 3. Messages

### `chat-send` (client → server)

```json
{"type": "chat-send", "group": "room-KFQB-12", "boxes": {"<uid>": "<hex box>", "<uid>": "<hex box>"}}
```

One box per other member that has a group key (§4), keyed by the recipient's uid (so at most 3). mm refuses (`chat-error`, nothing forwarded) when: the group is not the sender's; a box is addressed to the sender, to a non-member or to a member without `kx`; a box is not hex, is shorter than 114 bytes (the smallest box that can hold a message: nonce, MAC, 73 bytes of plaintext header and one byte of text) or longer than 512 bytes; there are no boxes, or more than 3; or the sender is over the rate limit (5 messages in 5 s, 30 in a minute, per account: "You're sending messages too fast."). Refused messages do not count toward the limit. mm does not require a box for every member with a key (one may have published its key while the message was on its way). Otherwise each recipient gets (the box as lowercase hex):

### `chat-msg` (server → client)

```json
{"type": "chat-msg", "group": "room-KFQB-12", "from": "<uid>", "box": "<hex box>"}
```

### `chat-report` (client → server)

```json
{"type": "chat-report", "group": "room-KFQB-12", "from": "<reported uid>", "reason": "",
 "messages": [{"seq": 7, "text": "…", "sig": "<128 hex>"}]}
```

Each message's `text` is the text exactly as it came out of the box (before the receiver's cleaner: that is what the signature covers), 1 to 300 bytes. At most 20 messages, `reason` (optional) at most 200 bytes; `from` is not the reporter; the group need not be the reporter's current one (the signatures name it). The whole request must fit mm's 8 KiB packet limit, which 20 messages of 300 bytes do not: a client sends the newest messages that fit. 5 reports an hour per account ("Too many reports. Try later."). mm answers `{"type": "chat-reported", "from": "<uid>"}` and stores the report in `chat_reports` after checking each message's signature (§4) against the reported account's identity keys: `verified` counts the messages that check out. A message that doesn't verify is kept but marked. Each message is stored as `{seq, text, raw, sig, verified}`: `text` cleaned (§5), `raw` the reported bytes as hex (they may hold NUL, which jsonb can't). A `from` that is no account is stored as NULL. `admin chat-reports` lists the unhandled reports, `admin chat-report-done <id>` takes one off the list.

### `chat-error` (server → client)

`{"type": "chat-error", "op": "chat-send", "error": "You're sending messages too fast."}`. The connection stays. `op` is the request's type (`chat-key`, `chat-send`, `chat-leave`, `chat-report`), or `invalid` for another `chat-*` type.

| Situation | `error` |
|---|---|
| The connection's `hello` had no valid `chatKey` | `Update the game to chat.` |
| The request names a group the sender is not in | `You are not in this chat.` |
| A box for the sender or a non-member | `That player isn't in this chat.` |
| A box for a member without `kx` | `That player can't read chat yet.` |
| Malformed (types, lengths, hex, uids, group id charset, a report about oneself) | `Invalid chat message.` |
| `chat-send` over 5 in 5 s or 30 a minute | `You're sending messages too fast.` |
| `chat-report` over 5 an hour | `Too many reports. Try later.` |
| `chat-key` / `chat-leave` over 10 in 10 s | `Too many requests. Wait a moment.` |

## 4. Crypto

Monocypher 4 in Dolphin (`crypto_x25519`, `crypto_blake2b`, `crypto_aead_lock`/`unlock`, `crypto_ed25519_*`); `x25519-dalek`, `blake2`, `chacha20poly1305` (`XChaCha20Poly1305`) and `ed25519-dalek` in Rust. `‖` is concatenation, `0` a single zero byte, strings are their ASCII/UTF-8 bytes without terminator, `G` the group id, `u64le` 8 little-endian bytes.

| What | Definition |
|---|---|
| Group key signature | `kxSig = Ed25519.sign(identity, "brawlonline-chat-kx-v1" ‖ 0 ‖ G ‖ 0 ‖ kx)` |
| Pair key | `s = X25519(own kx secret, their kx)`, refused if all zero; `key = BLAKE2b-256("brawlonline-chat-key-v1" ‖ 0 ‖ G ‖ 0 ‖ s ‖ lo ‖ hi)` with `lo`, `hi` the two `kx` public keys in byte order |
| Message signature | `sig = Ed25519.sign(identity, "brawlonline-chat-msg-v1" ‖ 0 ‖ G ‖ 0 ‖ from ‖ 0 ‖ u64le(seq) ‖ text)` |
| Plaintext | `0x01 ‖ u64le(seq) ‖ sig(64) ‖ text` (text 1..300 bytes, the rest of the plaintext) |
| Associated data | `"brawlonline-chat-box-v1" ‖ 0 ‖ G ‖ 0 ‖ from ‖ 0 ‖ to` |
| Box | `nonce(24, random) ‖ mac(16) ‖ XChaCha20(key, nonce) ⊕ plaintext` (Monocypher's `crypto_aead_lock`; RustCrypto's output is `ciphertext ‖ tag` and is reordered) |

`seq` starts at 1 in each group and goes up by one per message; the same plaintext (one signature) is encrypted for every recipient. A receiver drops a message whose `seq` is not above the last one it accepted from that sender in that group. A report's messages are verified with the message signature alone, so any recipient's copy proves who sent the text in which group.

Signatures are checked strictly (RustCrypto's `verify_strict`: no small-order keys or `R`, canonical `S`); honest Monocypher signatures always pass. Ed25519 signing is deterministic (RFC 8032), so the vectors below are exact. Monocypher 4's `crypto_aead_lock` is libsodium's `crypto_aead_xchacha20poly1305_ietf` with the MAC written separately, and `crypto_blake2b(hash, 32, …)` is BLAKE2b with a 32-byte digest (not a cut BLAKE2b-512).

Test vectors: `server/crates/common/src/chat.rs` (`test_vectors`) generates them from fixed keys and nonces (`cargo test -p common test_vectors -- --nocapture` prints them); `Source/UnitTests/Core/Online/ChatCryptoTest.cpp` checks Dolphin against the same bytes. They were also checked against libsodium (PyNaCl 1.5). `bytes(a..)` is the bytes a, a+1, a+2, …

| Input | Value |
|---|---|
| Sender identity seed | bytes(0x00..), 32 |
| Recipient identity seed | bytes(0x20..), 32 |
| Sender group key secret | bytes(0x40..), 32 |
| Recipient group key secret | bytes(0x60..), 32 |
| Nonce | bytes(0x80..), 24 |
| G | `room-KFQB-12` |
| from (sender) | `11111111-1111-4111-8111-111111111111` |
| to (recipient) | `22222222-2222-4222-8222-222222222222` |
| seq | 1 |
| text | `hello ｗｏｒｌｄ ねこ` (UTF-8, 28 bytes) |

| Output | Hex |
|---|---|
| Sender identity key | `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8` |
| Recipient identity key | `29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7` |
| Sender kx | `79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a` |
| Recipient kx | `675dd574ed7789310b3d2e7681f3790b466c773b1521fecf36577958371ea52f` |
| Sender kxSig | `47ec39ad5b0e785b3ad40d3afb9d876439e9cbfc6ccc4c36bc8e0e2778b11581ff51f27cce788697fdf08e404a897a7a4fff7b0b2f084cbfab00288ce939250d` |
| Pair key | `b58346539534a6603561da2c02479171669efd69a12ccc8114ba6d570ec54fc5` |
| Message sig | `655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a` |
| Plaintext | `010100000000000000655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a68656c6c6f20efbd97efbd8fefbd92efbd8cefbd8420e381ade38193` |
| Associated data | `627261776c6f6e6c696e652d636861742d626f782d763100726f6f6d2d4b4651422d31320031313131313131312d313131312d343131312d383131312d3131313131313131313131310032323232323232322d323232322d343232322d383232322d323232323232323232323232` |
| Box | `808182838485868788898a8b8c8d8e8f90919293949596972006179349a1893a3f0f1207a3f24df6a4210813228f3094a7319f3945640c4c55a3a6b54d78bff9f56e520e248754480102d74ec7034e692b38caddc70c057d50bff23530fa4e1d4f4a563f23b3e9b4eecd2101486f3a1a5a0111b29cbdd0d3168dc69757a22428c3b191ac6cb7289fd9dd2f6722` |

## 5. Text

Both sides apply the same cleaner (`Online::ChatText::Clean`, `common::chat::clean_text`) to what is typed before sending and to what is received before showing it. The steps run in this order, each on the result of the one before:

1. Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF, truncated sequences): the message is dropped.
2. Removed: C0 and C1 controls and DEL; U+00AD; U+034F; U+061C; U+115F, U+1160; U+17B4, U+17B5; U+180B..U+180F; U+200B..U+200F; U+2028..U+202E; U+2060..U+206F; U+3164; U+FE00..U+FE0F; U+FEFF; U+FFA0; U+FFF0..U+FFFB; U+E000..U+F8FF; U+1D173..U+1D17A; U+E0000..U+E0FFF; planes 15 and 16; noncharacters (U+FDD0..U+FDEF and every U+xFFFE, U+xFFFF).
3. Whitespace (U+0020, U+00A0, U+1680, U+2000..U+200A, U+202F, U+205F, U+3000) runs become one space (U+0020); leading and trailing spaces go. Tab, CR and LF are controls: step 2 removes them, so they do not separate words (`a\tb` is `ab`). Characters step 2 removed do not split a run (`a`, space, U+2060, space, `b` is `a b`).
4. Combining marks (U+0300..U+036F, U+0483..U+0489, U+0591..U+05BD, U+0610..U+061A, U+064B..U+065F, U+0E31..U+0E3A, U+0E47..U+0E4E, U+1AB0..U+1AFF, U+1DC0..U+1DFF, U+20D0..U+20FF, U+302A..U+302F, U+3099, U+309A, U+FE20..U+FE2F): at most 2 in a row, the rest dropped. Any other character, a space included, ends a run; characters step 2 removed do not.
5. At most 100 code points and 300 bytes: the first code points that fit both (cut on a code point boundary). A space the cut leaves at the end goes too (`"a" × 99 + " b"` gives `"a" × 99`), so cleaning twice changes nothing. Empty: dropped.

Names (from mm) go through the same steps with a limit of 32 code points (and the same 300 bytes).

## 6. Dolphin

- **Rooms thread** (`Online/Rooms.cpp`) owns the connection: it passes every `chat-*` message to `Online::Chat::OnServerMessage` and sends what `Online::Chat::TakeOutbox` returns. Encryption, signing and checks run there, never on the CPU or GPU thread.
- **Window** (`VideoCommon/OnlineChatUI.cpp`, drawn with the OSD): shown while the player is in a room group, or in a match group whose session is connected. Hidden while a match runs if "Show during matches" is off (`[Online] ChatInMatches`). Position, size and collapsed state: `[Online] ChatWindow`, fractions of the window, so a resized window keeps the layout.
- **Typing:** the "Activate NetPlay Chat" hotkey (default T) focuses the box; Enter sends and gives the game its keyboard back; Esc leaves the box. While the box has focus, `ciface::SetKeyboardMouseBlocked(true)` makes every keyboard and mouse control read 0, for the game and for hotkeys; the block is held until Enter and Esc are released.
- **Hidden players:** `<User>/Online/chat-hidden.json`, `{"hidden": ["<uid>", …]}`, at most 500.
- **Harness:** `chat_status`, `chat_send`, `chat_hide`, `chat_report`, `chat_leave` (`docs/harness-protocol.md`).
