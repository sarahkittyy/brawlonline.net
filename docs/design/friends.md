# Friends (design)

_2026-11-14. Sarah's board card ("i think we should add a friends list"), two rounds of mockups and her answers. Server: `server/crates/accounts/src/friends.rs`, migration `0036_friends.sql`. Launcher: `launcher/src/social/` (main process), `launcher/src/renderer/pages/home/overview/online_players/` (the Online / Friends panel), `components/social/` (pill, menu, pop-ups). Wire format: `docs/social-protocol.md` §7 and `docs/rooms-protocol.md` (`room-public`)._

## 1. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | The panel | Home's Online panel has two tabs, **Online** and **Friends**, in Home's own tab style (not the green title font: two tabs, the icons and a count did not fit the 298 px column). Each tab shows its players online after the chat's green dot ("Online ● 11", "Friends ● 5"); the Friends tab has its pending requests in an amber badge. Narrow columns (under 280 px) drop the counts first. The magnifier opens a search box **under the header** that filters the tab's list. |
| 2 | Online tab | Everyone online, **friends first**, no game state. A friend is marked by the friends glyph (two people) in the accent colour: at the bottom-right corner of their main's stock icon, in the small dot's place when they show no main, or right after their code without a disc (no main column). Sarah, 2026-11-14: "a tiny tiny friends icon on the corner of the name or on the corner of the character"; no circle around it. |
| 3 | Friends tab | Requests to the player first (accept / decline), then the friends online with what they are doing (a room the player can see, with **JOIN** or **FULL**; a game others can watch, with **WATCH**; else "In the launcher"), then **offline friends** (for everyone, unlike the admin-only offline players; the section folds), then the player's own requests waiting for an answer (**Cancel**). |
| 4 | Adding by code | The Friends tab's person+ button opens a box under the header for a code (or a profile address or link): Send asks them; the result shows under it ("Request sent to amy (AMY#560).", "No player has the code ZZZZ#1."). An X beside Send, Esc and the person+ button close it. A search for a code that is no friend's offers "Send a friend request to ABC#12". |
| 5 | Elsewhere | The right-click menu: **Accept / Decline friend request at the top**, a line under them; **Add friend** or **Cancel friend request** after Invite to game; **Join their room** for a friend in a room the player can join; **Remove friend** above Ignore user. The chat window's title has a small pill: "+ Add", "Sent", "Accept", or the friends icon (a click offers Remove friend). A friend's chat tab has the friends icon. A profile has the same pill **right next to the name** ("Add Friend", "Sent", "Accept", "Friends"), a developer's `</>` after it. |
| 6 | Icons | MUI's PersonAddAlt1 (add), PersonRemoveAlt1 (remove), PeopleAlt (friends, everywhere: a friend, a friends-only room, "you're friends"), and our own person with a clock (`PendingFriendIcon`) for a request waiting. |
| 7 | Requests | Stored on the server, sendable while the other player is offline, **never expire**. A request that arrives while the launcher is open pops up like an invite (bottom right, Accept / Decline / X), but **doesn't time out**; X leaves it in Friends > Requests. "X accepted your friend request." is a toast that goes away by itself. |
| 8 | Social rules | **Declining is silent**: the sender still sees "Sent", and sending again doesn't ask again. A request from someone the player ignores is kept but never shown and nobody is told. **Ignoring a friend ends the friendship** and drops the requests both ways. Two players asking each other are friends at once. A player can't ask someone they ignore. Limits: 500 friends, 100 requests waiting, 10 requests a minute and 30 an hour. |
| 9 | Ignore list | **On the account** (it crosses computers). The launcher uploads its install's `chat-hidden.json` once per account and keeps writing that file for the game (§3). |
| 10 | Rooms | A room is **PUBLIC, FRIENDS or PRIVATE**; the host's button in the room CSS goes in that order. A friends-only room is listed in Home's public rooms to **the friends of anyone playing in it** (Sarah's choice over "the host's friends"), with the friends icon before its name; it joins by code like a private room. A set that waits for a player makes the room private and gives it back its Friends afterwards. |
| 11 | Not shown | No friend list or count on public profiles (launcher or web), nothing in matchmaking, in-game chat or name tags, no "came online" toasts, no last-seen time for offline friends, no highlight of friends' names in the rooms list. Private rooms stay private: a friend in one is "In the launcher". |

## 2. Server

Tables (`0036_friends.sql`): `friend_requests (from_uid, to_uid, created_at, declined)`, `friendships (lo, hi, since)` (one row per pair, the smaller uid first), `ignores (uid, ignored, created_at)` and `users.ignores_imported`. Everything is by uid, so a rename keeps friends.

Endpoints (session auth; answers are `GET /v1/friends`'s lists unless said):

| Request | Notes |
|---|---|
| `GET /v1/friends` | `{friends, incoming, outgoing}`, each `[{uid, displayName, connectCode, country?, main?, since}]`. Friends by name; requests newest first. `incoming` leaves out declined ones and players the caller ignores; `outgoing` keeps declined ones (the sender isn't told). Banned accounts are left out. |
| `POST /v1/friends/requests {to}` | `to`: a code (`ABCD#123`, any case), a profile address (`abcd-123`) or link, or a uid. `{status: "sent" \| "friends", player, ...lists}`. 404 `no_player` "No player has the code …", 400 `invalid` (yourself), 409 `ignoring`, `friends_full`, `too_many_requests`, 429. |
| `POST /v1/friends/requests/{uid}/accept` | 404 `no_request`; 409 `friends_full` (either list). |
| `POST /v1/friends/requests/{uid}/decline` | Marks it declined. Nobody is told. |
| `DELETE /v1/friends/requests/{uid}` | Takes back the caller's own request. |
| `DELETE /v1/friends/{uid}` | No longer friends. |
| `GET /v1/me/ignores` | `{ignored: [uid], imported}`. |
| `PUT /v1/me/ignores/{uid}`, `DELETE /v1/me/ignores/{uid}` | Ignoring ends a friendship and the requests both ways. At most 500 (the game's file holds 500). |
| `POST /v1/me/ignores/import {uids}` | The launcher's one upload per account; once `imported`, it changes nothing. Unknown uids are skipped. |

Each change tells the launchers concerned on their social stream: `{"type": "friends", "change": "request" | "accepted" | "changed", "player"?}` (`docs/social-protocol.md` §7).

Friends-only rooms: mm keeps a room's `friends` flag beside `public` (`room-public {public, friends}`); its status snapshot lists friends-only rooms in an internal `friendRooms`, which accounts never serves. `GET /v1/rooms` puts in front of the public rooms the friends-only ones the caller plays in or a friend of theirs plays in (`friends: true`), from each account's friends kept in memory (`FriendCache`, dropped when they change).

## 3. The ignore list on the account

The game reads and writes `<User>/Online/chat-hidden.json` (in-game Ignore); the launcher keeps a per-account copy of the list as it last put the two in step, `chat-hidden-synced-<uid>.json`. On connecting:

- the account was never uploaded (`imported: false`): the file goes up (`POST /v1/me/ignores/import`);
- the copy is there: what the game changed since it (added, removed) goes up;
- no copy (another computer, or the first look since updating): only what the file adds goes up; nothing is removed.

Then the account's list is written to the file and to the copy. While connected, a change the game makes to the file (seen within 2 s) goes up the same way, and the launcher's own Ignore / Unignore goes straight to the account (the file follows). Without the server the file is the list, as before, and its changes go up at the next connect.

## 4. The game

PPOM: `RF_FRIENDS = 0x40` in the room flags (without `RF_PUBLIC`); `ROOM_PUBLIC` takes 0 private, 1 public, 2 friends only. The room CSS's ITEM button reads PUBLIC / FRIENDS / PRIVATE (`room_css.cpp`, a third label copy); L or the button steps PUBLIC → FRIENDS → PRIVATE → PUBLIC. Dolphin passes `room-state`'s `friends` into the flag and `friends: true` with `room-public`. An older game or Dolphin sees a friends-only room as private.
