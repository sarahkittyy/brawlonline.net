# Twitch live

Players who stream Brawl Online on Twitch show as live around the launcher (2026-11-24, Sarah's board; mockups
in `run/design/twitch-live`, round 1 and her notes on it).

## Who is live

**Connected, not typed** (Sarah's board: "make the profile connect only"). Anyone can type anyone's handle, so a
profile shows Twitch only for a connected account, and edit mode has no Twitch text box. A handle typed before (64
of 1,237 profiles on 2026-11-24) stays stored but is never shown: it only gives that player the hint to connect
(below); an old launcher can still store one the same way. A player connects their Twitch account through Twitch's own sign-in (OAuth authorization
code, no scope): the launcher asks `POST /v1/me/twitch/connect` for the address and opens it in the browser;
Twitch sends the player back to `GET /v1/twitch/callback` (`<PUBLIC_BASE_URL>/v1/twitch/callback`, a plain page:
"You're connected!" or "Not connected."); accounts trades the code for a token, reads who signed in
(`/helix/users`), revokes the token and keeps only the Twitch user id, login and display name
(`twitch_links`, migration 0045). The profile's Twitch handle becomes that login. One Twitch account belongs to
one Brawl Online account: connecting it from another moves it. `DELETE /v1/me/twitch` disconnects (the typed
handle stays). The launcher reads `GET /v1/me/twitch` (`available`, `linked`, `handle`) every few seconds while a
connect waits for the browser.

**Live = streaming while playing.** Twitch's category is not looked at: people stream Brawl Online under Brawl,
Melee, Project+ or anything else, and Twitch can't tell us which game is on screen. Instead accounts notes every
10 s who mm says is playing (in a room, P+ or Brawl Mods; searching; in a match; in a live game) and once a
minute asks Twitch (`/helix/streams`, an app token from client credentials, 100 ids per call) which of the
connected accounts seen playing in the last 10 minutes are live (the 10 minutes cover the menus between sets).
`GET /v1/twitch/streams` lists them, most viewers first: the player (name, code, shown main) and the stream
(login, Twitch name, title, category, viewers, picture at 440x248 with a 5-minute cache buster, start).

Without `TWITCH_CLIENT_ID` / `TWITCH_CLIENT_SECRET` it is all off: no streams, connecting answers 503 and the
launcher shows no Connect account. A staging server with `STAGING_FAKE_STATE` lists the file's `twitch` streams
(`tools/staging/seed_fake.py`: Marlo, Knuckles, MayoTayo1, their live game's stage as the picture) and connects
without Twitch (the typed handle, or the name, becomes the account).

## Where it shows (Sarah's picks)

- **Online list:** Twitch's logo beside the flag (it opens the stream). Folded rail: the logo over the friend's
  picture's top-right corner. Always Twitch's purple on nothing, never in a square ("you'd just use the purple
  fg / transparent bg big icon for both").
- **Hover card and profile:** the stream (picture with the live dot and viewers, Twitch name, title, category)
  under the mains; the profile's opens the stream.
- **Spectate:** a Twitch row at the top, above the featured game, while there are streams; cards most
  viewers first; "(logo) Twitch" on the left and Connect account on the right for a player who hasn't connected.
  Without streams the row shows for that player too, its heading and Connect account only (Sarah's board: the
  button "should be there, somewhere").
  The logo after live players' names in the list and the featured game. The featured game is a little smaller.
  A card's picture and title open the stream; the player's name under the picture is a name as anywhere else
  (hover card, profile on a click, player menu on a right click), so the card itself is no button.
- **Sidebar:** Twitch's logo before Spectate's live count while there are streams (not when folded).
- **Connect account** is also in Settings > Social ("Twitch", then Connect account or the account and Disconnect,
  as the other settings' small outlined buttons) and in the profile's edit mode, where the Twitch row keeps the
  other rows' shape: the logo, then a box the size of their text boxes that is Connect account, or the connected
  account with Disconnect at its right.
- **The hint:** a player with a typed Twitch handle who never connected gets the profile glow back on this
  client: the sidebar's card, then the pencil, then Connect account. Clicking the pencil ends it, connected or
  not (`TWITCH_CONNECT_HINT_DONE`).

## Production

Sarah registers an application at dev.twitch.tv/console (category Website Integration) with OAuth Redirect URL
`https://brawlonline.net/v1/twitch/callback`, and puts its Client ID and a Client Secret in the accounts env as
`TWITCH_CLIENT_ID` and `TWITCH_CLIENT_SECRET` (systemd EnvironmentFile; restart pp-accounts). nginx already sends
`/v1/` to accounts. Twitch's API is free; one `/helix/streams` call a minute per 100 connected players who are
playing is far under its limits (800 points a minute).

New strings for Sarah's review: "Twitch", "Connect account" (hers), "Disconnect", "Twitch: @login" (the profile's
social link format), the callback page's "You're connected!" (hers, from the start.gg link page) and
"Not connected.".
