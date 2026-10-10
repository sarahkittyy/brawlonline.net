# Chat: ideas (draft, superseded)

_Superseded 2026-10-10 by `docs/design/chat.md` (the decisions) and `docs/chat-protocol.md` (what was built): option B, with mm relaying end-to-end encrypted messages instead of a peer-to-peer channel._

_2026-10-08. For the user to pick from before anything is implemented._

## What exists today

- Nothing works yet. The CSS status line doesn't show "Use D-Pad to Chat" (`docs/game-code.md` §6, screen 10: "not started").
- The plumbing is half-designed: Dolphin already gets each user's 16 default quick-chat strings from `GET /user/{uid}?additionalFields=chatMessages,rank`. The design puts a chat byte in the per-frame input (`docs/backend-design.md`: "Chat and hold Z to disconnect become one extra byte per player per frame") and each player's 16 strings in SESSION.
- The launcher dropped Slippi's Settings > Chat page, because there was no server endpoint and Slippi's paid message options (`launcher/PPLUS_PORTING.md` §1).

## How others do it

| | Slippi | Orca / YouGame |
|---|---|---|
| Kind | **Quick chat only**: 16 preset messages, no free text | Free text plus room notices, in YouGame's own overlay (Roboto font, deliberately not the game's look) |
| Input | D-pad: the first press opens a category (Up/Left/Right/Down), the second sends; B cancels; the window closes after 2.5 s; 0.2 s minimum gap | Typed in the app page around the game window |
| Where | On the CSS, plus on-screen notifications in game (at most 4 of yours, 10 in total) | Lobby, and the results screen stays open 5 min after a ranked set so players can talk |
| Setting | Enabled / Direct only / Disabled. A disabled player auto-replies "player has chat disabled" | — |
| Custom messages | 16 strings, edited in the launcher | — |
| Abuse | Nothing to moderate: only preset strings | Free text, through YouGame's servers |

## Options

### A. Slippi quick chat (parity)

```
 CSS, after connecting:            D-pad Up pressed:
 +---------------------------+     +---------------------------+
 | Playing: SARA#996         |     |  ^ ggs          > one more|
 | Press START to lock in    |     |  < well played  v brb     |
 | Use D-Pad to Chat         |     |                           |
 +---------------------------+     +---------------------------+
 Incoming: a text line in the game's font in the CSS pane; in a match, a small notification line.
```

- In game, controller only, so it works on Steam Deck, a GameCube adapter, anything. Text uses Brawl/P+ fonts in existing panes (no new art).
- Settings > Chat comes back in the launcher (Slippi's page) to edit the 16 messages, free for everyone. Chat setting: Enabled / Direct only / Disabled.
- Transport is already designed: one input byte per frame, so messages are confirmed and replayed like pads (and they'd appear in replays for free).
- Safe: the receiver only accepts ids 0–15 (plus "chat disabled"); the strings come from the server, length-limited and filtered at save time.
- Cost: game-code UI (a category window on the CSS, notifications in the match), the Dolphin input byte, the server endpoint for custom messages, and the launcher settings page. Small to medium.

### B. Free text in an in-game overlay (ImGui popup)

```
 +--------------------------------------------------+
 |                                                  |
 |   [CSS / results screen]                         |
 |                                                  |
 |  SARA#996: one more?                             |
 |  you: sure, fd this time                         |
 |  > _                              (Enter: send)  |
 +--------------------------------------------------+
```

- Dolphin draws it with its existing ImGui OSD layer, in the game's font loaded from the disc (Orca does the same for its "kit").
- Opening it: **Enter** (or **T**) on the CSS, the stage select and the results screen. Esc closes. While it's open, keyboard input goes to the box, not the game. On Deck, Steam's on-screen keyboard (Steam + X).
- In a match it shows incoming messages only and never takes focus, so you can't type by accident. Maybe it's hidden entirely in Ranked.
- Transport: a reliable side channel on the existing peer connection, never the input stream (text doesn't belong in the rollback inputs). Limits: 120 UTF-8 bytes, printable characters only, at most 1 message/s, validated as hostile data on receipt.
- Needs mute (per player, remembered), a "text chat: on/off/friends only" setting (default off for Unranked and Ranked?) and a report button that stores the last N lines locally for a report.
- Cost: medium. Moderation is the real cost, not the code.

### C. Chat in the launcher (separate window)

```
 Launcher (beside or behind Dolphin)
 +--------------------+
 | Match: SARA#996    |
 | ------------------ |
 | SARA#996: ggs      |
 | you: gg!           |
 | [type here.......] |
 +--------------------+
```

- Lives in the launcher, a docked panel or a small always-on-top window. Dolphin tells the launcher who the opponent is over a local IPC channel. Messages go through our server over a WebSocket.
- Could later carry friends lists, DMs, invites and Teams lobbies (useful for drop-in and Teams).
- Downsides: invisible in fullscreen, awkward on Deck (Game Mode shows one window), needs a server-side chat relay with moderation, logs and storage, and it's the furthest from Slippi.
- Cost: large (server plus launcher plus IPC).

### D. Keyboard shortcut quick chat

- Like A, but F1–F4 (or 1–4) send preset messages, for keyboard players. Cheap if A exists: the same message ids, a second input path. Not a separate option, more an add-on to A.

## Recommendation

1. **Build A first.** It's Slippi parity, controller friendly, has no moderation burden, works in replays, and the transport is already designed. Add D's hotkeys if people want them.
2. **Then B, opt-in**, for Direct (friends) first. Only consider it for Unranked/Ranked once mute and report exist.
3. **Leave C** until there's a reason the launcher needs a social layer (friends list, invites, drop-in or Teams lobbies). If the drop-in design ends up with a launcher-side lobby, revisit it then.

## Questions for you

1. A first, or do you specifically want free text (B) from the start?
2. Should chat work during matches (notifications), or only on the CSS and between games?
3. Free text in Ranked/Unranked against strangers: never, opt-in, or opt-out?
4. Custom quick-chat messages: anyone can edit all 16, or a fixed list plus a few custom ones (less to filter)?
