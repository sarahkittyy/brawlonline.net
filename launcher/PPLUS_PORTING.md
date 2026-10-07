# Porting the Slippi Launcher to Project+

This repository is the Slippi Launcher (GPL-3.0) forked with its full history (branch `main-pplus`, upstream `0930a2b6`). The goal is Slippi's launcher UX one-to-one: same screens, flows and features, pointed at our own services, our Dolphin fork and Project+. No new UI or flows were invented; the few places where a field or setting had to be added are listed under "Adaptations" below.

The product name is not decided: everything user-visible reads `PRODUCT_NAME` (placeholder `PlusOnline`) from `src/common/product.ts`, and the hosts default to subdomains of `fluffycat.gay` there. A unit test (`src/common/product.test.ts`) checks that `package.json`, `release/app/package.json` and `electron-builder.json` agree with the constant.

## 1. Inventory of Slippi-specific integrations

Decision: **Replace** (same feature, our implementation), **Keep** (unchanged or re-pointed), **Drop** (removed for now; the reason is given).

| Integration | Slippi | Decision | What we did |
|---|---|---|---|
| Firebase auth (email/password, multi-account, persistence) | `services/auth/*` with one Firebase app per account | **Replace** | Our accounts API (`server/crates/accounts`). HTTP calls run in the main process (`src/accounts/`); session tokens are stored per account in `userData/sessions.json`, encrypted with Electron `safeStorage`. The renderer services keep Slippi's interfaces (`AuthService`, `MultiAccountService`, up to 5 accounts, switching, session-expired re-login). |
| Sign-up (`createUserNew` callable) | email, display name, password | **Replace** | `POST /v1/auth/signup`, plus the **invite code** field added to the existing sign-up form. |
| Email verification | Firebase `sendEmailVerification`, `reload()` | **Replace** | Server sends the mail at sign-up; "Check verification" re-reads `GET /v1/me`; "Send again" calls `/v1/auth/verify-email/resend`. Duplicate sends within 30 s are suppressed (the server already sent one at sign-up, Firebase did not). |
| Password reset | Firebase `sendPasswordResetEmail` | **Replace** | `POST /v1/auth/password-reset/request`. The emailed link opens the server's own reset page. |
| GraphQL `getUser` (connect code, `private.playKey`, `rulesAccepted`) + `getLatestDolphin.version` | Apollo client, `SLIPPI_GRAPHQL_ENDPOINT` | **Replace** | `GET /v1/me` (its `userJson` becomes the play key; `latestVersion` comes from the server). `services/slippi` was renamed `services/backend`. |
| `validateUserIdQuery` | GraphQL | **Replace** | `GET /user/{uid}` (public users-rest shape). |
| Ranked profile (`rankedNetplayProfile`) and tier calculation | GraphQL + `calculate_rank.ts` | **Keep logic, replace source** | Rank comes from `GET /user/{uid}` `rank` (all zero until ranked exists, so the widget shows "No ranking"). Tier thresholds unchanged. Rank badge art dropped: the tier is shown as text only (backend-design.md 2.6). |
| `userRename`, `userAcceptRules`, `userInitNetplay` | GraphQL mutations | **Replace** | `POST /v1/me/rename`, `/v1/me/accept-rules`, `/v1/me/netplay`. |
| Chat message settings (`GetChatMessageConfigData`, `userSetChatMessages`, paid messages) | Settings > Chat | **Drop** | No server endpoint yet and the paid options are Slippi's subscription. Dolphin gets the 16 default messages from `GET /user/{uid}`. Re-add the page when the server stores custom messages. |
| Subscriptions, support badge, Ranked Day, ad dialog, donate link | Home + header | **Drop** | Slippi's paywall; design decision D (backend-design.md 1.6). |
| `user.json` (play key file) | Written on Play if different, `{uid, playKey, connectCode, displayName, latestVersion}`, never deleted by the launcher | **Keep** | Same content, same key order, same write-on-Play logic. Location: `<Dolphin User folder>/Online/user.json` (Slippi: `<User>/Slippi/user.json`). Verified identical to the server's `GET /v1/me/user-json`. |
| Dolphin download and update (`getLatestDolphin`, Ishiiruka/mainline installations, beta channel, promotion to stable) | `dolphin/install/*`, `manager.ts` | **Replace** | One local installation (`dolphin/install/local_installation.ts`) for the Dolphin build configured in Settings. Nothing is downloaded. `fetch_latest_version.ts` is a stub hook returning `null` for our future update channel; the installers (`download.ts`, `windows.ts`, `macos.ts`, `linux.ts`) are kept for it. The beta release channel setting is dropped (nothing to choose). |
| Netplay / playback Dolphin split | Two installations under `userData/netplay` and `userData/playback` | **Keep** | Both use our single build (playback defaults to the netplay executable); each has its own User folder under `userData/{netplay,playback}/User`, as on Slippi for Windows. |
| Play button / boot | `Dolphin -b -e <Melee ISO>`, optional "Boot to CSS" Gecko code | **Replace** | `Dolphin -u <User> -e "<User>/Launcher/Project+ Netplay Launcher.dol"` with `[Core] DefaultISO = <Brawl ISO>` in Dolphin.ini: P+'s own boot chain (launcher DOL applies the GCT from the virtual SD card, then boots the disc). No `-b`. Boot to CSS is a Melee code and is dropped. "Launch Dolphin" (Play button action) is kept. |
| Gecko code manager | Edits `GALE01` (Melee) codes | **Keep, retarget** | Edits `RSBE01` (`Sys/GameSettings/RSBE01.ini` + `User/GameSettings/RSBE01.ini`). P+'s own codeset (GCT on the SD card) is not touched. |
| Synced Dolphin settings (replay folder, save replays, monthly folders, jukebox) | `[Slippi]` section of Dolphin.ini | **Keep, rename section** | Same keys in `[Online]`. Jukebox (Melee HPS music) dropped. |
| ISO verification | SHA-1 table of Melee images, "unknown ISO, use anyway" | **Replace** | MD5 of exactly NTSC-U Brawl Rev 1 `d18726e6dfdc8bdbdad540b561051087` and Rev 2 `52ce7160ced2505ad5e397477d0ea4fe`; a header check (`RSBE01`, revision 1/2) rejects other files instantly. Everything else is **Invalid** (no "use anyway"), and Play refuses an invalid ISO with Slippi's message style. Results are cached by path, size and mtime (hashing 8.5 GB takes ~25 s). |
| Broadcast / spectate (Slippi WebSocket relay, `SLIPPI_WS_SERVER`) and Spectate Remote Control (localhost:49809) | `src/broadcast`, `src/remote`, Spectate page, settings | **Drop** | No relay server yet (backend-design.md 1.9: D for the first release). |
| Console mirroring (Wii + Slippi Nintendont, OBS switching) | `src/console`, Console Mirror page | **Drop** | Melee/Nintendont only. |
| News feed (Bluesky, GitHub releases, Medium), upcoming tournaments (meleemajors.gg, smash-map.com, ip-api.com location) | Home tabs, `main/content_management`, `main/fetch_cross_origin` | **Drop** | Slippi/Melee content sources (backend-design.md 1.10). The Home page keeps its Overview tab with the ranking widget. "Location access" setting dropped with it. |
| Telemetry / Sentry | none in the launcher | — | Nothing to remove; no analytics added. Logs stay local ("Copy logs"). |
| Launcher self-update (electron-updater, GitHub `project-slippi/slippi-launcher`) | `main/app_updater.ts` | **Keep, re-point** | Same hooks and UI. Feed: `generic` provider at `launcherUpdates` (`https://updates.fluffycat.gay/launcher`, `PPO_UPDATES_URL`); `electron-builder.json` publishes there. |
| `slippi://` URL scheme (downloads replays from Slippi's GCS bucket) | `main.ts`, NSIS registry keys | **Drop** | No replay storage. Opening a local replay file (file association) is kept. |
| Replay parsing, stats and indexing (`@slippi/slippi-js`, `.slp`) | Worker pool, SQLite, stats pages | **Replace (minimal)** | Our replay format is undecided. The browser lists `.rep`/`.json` files with name, size and date only (`src/replays/replay_format.ts`); stats pages report "not available". slippi-js stays a dependency for the stats types only. See section 6. |
| Network diagnostics (Google STUN, UPnP/NAT-PMP, CGNAT traceroute) | Help page | **Keep** | Unchanged (Google's public STUN). Later: our own STUN (backend-design.md 1.10). |
| Rosetta prompt (x86 Dolphin on Apple Silicon) | dialog | **Keep** | Wording no longer names Slippi Dolphin. |
| Rules & policies (rules version 1, privacy policy, ToS) | Quick start + Settings | **Keep, rebrand** | Rules text kept with the product name; policy links go to `<website>/privacy` and `/terms`, which do not exist yet (open issue). |
| Profile / manage-account links (slippi.gg) | User menu, replay stats, "Wrong email? Change email" | **Drop** | No website pages yet. |
| Discord/Bluesky links, footer, Help menu, support text | various | **Drop** | Slippi's community links. Support text now says to copy logs and send them. |
| Branding: name, logos, icons, colours, fonts (Rubik, Maven Pro), rank badges, Melee stock/stage images, bouncing logo, crown | everywhere | **Replace / Drop** | See section 5. `assets/` keeps only the macOS entitlements and the GameCube adapter driver installer. App and installer icons are Electron's defaults until we have art. |
| Package metadata, appId, NSIS installer, file association, AppImage names | `package.json`, `electron-builder.json`, `installer.nsh` | **Replace** | `PlusOnline`, `gay.fluffycat.plusonline`, `.rep` files, generic publish feed, no Slippi signing config or URL handler. |
| Translations (es, ja, pt, ru) | `locales/` | **Keep** | Re-synced with `i18n:sync`; Slippi-only strings were removed, changed strings fall back to English. |

### Adaptations to Slippi's UI (where the field set differs)

- **Invite code** field in the existing sign-up form (same filled `TextField` style, after "Confirm password").
- **Dolphin executable** path setting at the top of Settings > Dolphin > Netplay/Playback, in the place of the dropped "Release Channel" setting, using Slippi's existing `PathInput` component. Empty means the default, which is shown as the placeholder.
- The connect-code step's "Can be changed later for a one-time payment" now reads "Can be changed once later on request" (one admin-approved change, backend-design.md 1.3). The connect-code letters are chosen in Slippi's existing Activate Online step after sign-up, as before.
- ISO wording: "Brawl ISO", NTSC-U revision 1 or 2.

## 2. Accounts and `user.json`

```
renderer: LoginForm / VerifyEmailForm / ActivateOnlineForm / AcceptRules  (Slippi's components)
   -> AuthService + MultiAccountService + BackendService   (src/renderer/services, Slippi's interfaces)
   -> window.electron.accounts                              (preload, src/accounts/api.ts)
   -> AccountsManager + SessionStore                        (main process, src/accounts)
   -> https://accounts.fluffycat.gay  (PPO_ACCOUNTS_URL)    (server/crates/accounts)
```

- Errors come back as values, so the UI shows the server's own message ("Wrong email or password", "Sign-up needs an invite code"…) exactly where Slippi showed Firebase's.
- A 401 on a stored session deletes it and the UI asks for a login (Slippi's `SessionExpiredError` path for account switching). Offline with a stored session, the user stays logged in with the last known details, like Firebase's cached user.
- A server with `REQUIRE_EMAIL_VERIFICATION=false` reports `emailVerificationRequired: false`; the launcher then treats the email as verified and skips the step (backend-design.md section 3).
- `user.json` is written exactly as Slippi does: on Play, only when the content differs, as `JSON.stringify(playKey, null, 2)` with keys `uid, playKey, connectCode, displayName, latestVersion`. Path: `<Dolphin User folder>/Online/user.json`. **Contract for our Dolphin fork**: rename `SLIPPI_DIR` (`CommonPaths.h`) to `Online`, and read the synced settings from the `[Online]` section of Dolphin.ini (`DOLPHIN_ONLINE_DIR`, `DOLPHIN_INI_SECTION` in `src/common/product.ts`). Like Slippi's launcher, logging out does not delete `user.json`; Dolphin's in-game logout does.

## 3. Dolphin

- **Executable**: Settings > Dolphin, else `PPO_DOLPHIN_PATH`, else in development `../dolphin/build/release/x64/Binaries/Dolphin.exe` (`D:\code\pm_rollback\dolphin\build\release\x64\Binaries\Dolphin.exe` here), else `<userData>/netplay/Dolphin.exe` where a future installer will put it.
- **User folder**: `<install>/User` if the build is portable (`portable.txt` next to the binary), else `<userData>/{netplay,playback}/User`. Dolphin always gets `-u <User folder>`.
- **P+ files**: the netplay User folder needs `Launcher/Project+ Netplay Launcher.dol` and `Wii/sd.raw`. In development a new folder is seeded once from `../run/template-user` (`PPO_DOLPHIN_USER_TEMPLATE`), skipping `Load/`, `Logs/` and caches. Without them, Play shows which files are missing. A packaged release will need these files from our Dolphin release (open issue).
- **Update hook**: `src/dolphin/install/fetch_latest_version.ts` returns `null`. To enable updates, serve `GET /v1/dolphin/latest?purpose=&beta=` and return `{version, downloadUrls}` there; `DolphinManager` already has the download/install plumbing.
- **Soft/hard reset**: soft clears the cache; hard also removes `Config/`, `GameSettings/`, `Logs/` and `Online/` but keeps `Wii/` (SD card and saves) and `Launcher/`.
- **Version**: shown from `Dolphin --version`. Our build takes ~27 s to answer it, so Play no longer waits for it (Slippi did); it is fetched in the background and cached per executable.

## 4. Disc verification

`src/main/verify_iso.ts`: header check, then MD5 over the whole image; accepted: Rev 1 `d18726e6dfdc8bdbdad540b561051087`, Rev 2 `52ce7160ced2505ad5e397477d0ea4fe`. Verified on the real images: both report **Valid** (~26 s each, then cached); `sd.raw` and other files report **Invalid**. Compressed images (RVZ, WBFS, 7z) are refused up front with Slippi's message style.

## 5. Look and feel from the user's own game files

Rules: no AI-generated or invented art, no Slippi art, no generic icon pack as the identity, and no Nintendo assets in the repository or distribution.

- **Fallback** (no disc chosen, or extraction failed): system font, neutral greys (`styles/_variables.scss`, `styles/theme.ts`), no logo, Electron's default icon.
- **Extraction** (`src/brawl_assets`, `src/game_assets`): when an ISO is set (and on start-up if the sources changed), the main process extracts into `<cache>/theme` (development: `launcher/.asset-cache/theme`, gitignored; packaged: `<userData>/game-assets/theme`) and serves it to the renderer on the `game-asset://` scheme. Sources: the P+ SD card (`<netplay User>/Wii/sd.raw`, FAT32, `/Project+/pf/…`) first, then the disc. The disc is read from an extracted `DATA/files` folder in development, otherwise from the ISO via the user's own `DolphinTool extract` (so the launcher never contains the Wii common key).
- **Formats decoded** (all in TypeScript, no dependencies): LZ77 (0x10, 0x11, RLE 0x30), Brawl ARC, U8, BRRES, TEX0 + PLT0 with all GX formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C4, C8, C14X2), RFNT/BRFNT (FINF, TGLP, CWDH, CMAP methods 0-2), FAT32 (MBR or superfloppy, long names). PNG encoder/decoder and a TrueType writer (glyphs traced from the RFNT bitmaps) are included. Ported in part from BrawlCrate/BrawlLib (GPL-3.0, credited in the file headers and NOTICE).
- **Assets used** (`src/brawl_assets/catalog.ts`):

  | Role | Source | Texture |
  |---|---|---|
  | Page background | P+ `menu2/sc_selmap.pac` | `bg_gradient` (RGBA8) + `bg_grid` tile (I4) |
  | Button frame / selected frame | `menu2/mu_menumain(_en).pac` | `MenCmn00`, `MenSelchrEntryW01b` (IA4, nine-slice) |
  | Panel (dialogs) | `menu2/mu_menumain(_en).pac` | `MenSelchrEntryW01` (IA4, nine-slice) |
  | Cursor | `menu2/mu_menumain(_en).pac` | `MenSelmapCursorPly.1` |
  | Menu / title / small fonts | `system/font/font_latin1.arc`, `font_hira.brfnt`, `font_melee.brfnt` (P+ `system/common2/3.pac` hold the same) | RFNT → TTF |
  | Stock icons (44 with P+, 40 disc-only) | P+ `menu/common/StockFaceTex.brres`, disc `StockFaceTex_en.brres` | `InfStc.*` (C8/C4 + palette) |

  Logos (Smash Bros., Project+) are deliberately excluded.
- **Theme** (`src/renderer/styles/game_theme.tsx`): Brawl's menu frames are greyscale masks that the game tints at run time. The renderer tints them the same way (multiply), with colours sampled from P+'s own background gradient texture (teal top, dark middle, blue bottom), so every colour also comes from the game. Frames are applied with CSS `border-image` (nine-slice), fonts with `@font-face`. A disc-only extraction has no gradient and keeps the neutral colours.
- **Not replaced**: Material UI's functional glyphs (settings cog, search, check marks…) remain in both looks. They are not used as the product's identity, but they are a generic icon set; replacing them with game icons is an open question.

Screenshots taken during development (app window only, via the DevTools protocol) are in the session's scratchpad, not in the repository: fallback quick start, sign-up form with the invite code, verify email, accept rules, connect code, the themed "all set up" page, home, user menu, login dialog with a server error, Game and Netplay Dolphin settings, replay browser.

## 6. Replays

Our replay format is not decided. Until it is:

- The browser (Slippi's UI unchanged: folder tree, search, sort, "Hide short games") lists files named `*.rep` and `*.json` in the replay folders, with the date inferred from the file name (`Game_YYYYMMDDTHHMMSS.rep`) or the file's birth time. No players, stage or duration.
- The stats page reports that stats are not available; "Launch replay" starts the playback Dolphin with Slippi's `-i <comm.json>`, which our Dolphin does not implement yet.

What the browser needs from the format (to fill Slippi's existing UI): per game a start time, stage id, duration in frames, game mode, ranked/match id and game number, teams flag; per player port, character id, costume, team, display name and connect code, winner flag; for stats, a per-frame stream like slippi-js's. Plug a parser into `src/replays/replay_format.ts` (`readReplayFileInfo`) and fill `characterStockKeys` in `src/renderer/lib/utils.ts` to show the extracted stock icons. Stage images would also have to come from the disc.

## 7. Testing

- `npm run typecheck`, `npm run lint`, `npm test` (after `npm run build`, as upstream CI does): all pass. New tests cover the accounts client, session store and error handling, ISO verification, Dolphin paths, the asset cache manager and protocol, replay listing, product identity, and every decoder (synthetic data, plus integration tests on the real disc and SD card when present).
- End to end against the local `accounts` service (invite from `admin invite create`, file mailer): sign-up with invite → verification link from the mail file → check verification → accept rules → connect code `SARA#996` assigned → ISO verified → assets extracted and the theme applied without a restart → Play wrote `user.json` (identical to `GET /v1/me/user-json`) and started Dolphin with the P+ launcher DOL → log out → wrong password shows the server's error → log in → password reset request sends the reset mail. Session restore across restarts verified.

## Server issues

Found while porting; not fixed here (server/ is out of scope):

1. **Extractor rejections are not JSON.** A malformed or mistyped body gets axum's plain-text 422 (`Failed to deserialize the JSON body…`) and a missing content type a plain-text 415, instead of the documented `{"error": {"code", "message"}}`. The launcher copes (`parseError` falls back to the text), but other clients will not. Fix: map `JsonRejection` to `ApiError`.
2. **Endpoints the launcher would use that do not exist yet**: `GET /v1/dolphin/latest` (Dolphin update channel), chat-message configuration (`GetChatMessageConfigData` / `userSetChatMessages`), email change (Slippi's "Wrong email? Change email"), and web pages for `/privacy`, `/terms`, user profiles and account management (all 404). The accept-rules step links to `/privacy` and `/terms`.
3. **Rank is not in `GET /v1/me`.** Slippi's `getUser` returned the ranked profile with the user; we make a second public call to `/user/{uid}`. Harmless, but one round trip per refresh.
4. **Verification mail at sign-up.** The server emails at sign-up, while Slippi's flow sends the first mail from the Verify Email step. The launcher suppresses the extra send, but with `MAIL_DAILY_LIMIT` at 90 a resend rate limit/cool-down on the server side (it has one) is what really protects the quota.

## Dolphin fork issues

1. **P+'s GitHub auto-updater runs on every start** (`MainWindow::CheckForUpdatesAuto`, only skipped under the harness). It shows a modal "Update Available" dialog in the main window's constructor, which blocks the `-e` boot until it is dismissed, and closing it starts P+'s updater, which would overwrite our build with P+'s official release. The launcher owns updates (backend-design.md 1.10): remove or disable this in the fork. During testing the dialog appeared and Dolphin was closed before anything was downloaded.
2. `Dolphin.exe --version` takes ~27 s when spawned from Node (it prints `Project+ Dolphin <scm>`); Slippi's builds answer at once. Cause not investigated; worth a fast path.
3. Our build prints no semver; `latestVersion` in `user.json` (`0.1.0`) and the build's version cannot be compared yet.
4. Read `user.json` from `<User>/Online/` and the synced settings from `[Online]` (section 2).
5. Playback (`-i comm.json`) and replay recording do not exist yet (section 6).

## Open issues

- **Product name, icons, website**: placeholder name; no app/installer/tray icon (Electron default); privacy policy, terms, profile and account pages do not exist.
- **Packaging the P+ files**: the netplay User folder needs P+'s launcher DOL and `sd.raw`. Development seeds them from `run/template-user`; a release must ship or obtain them (P+'s files have no public license: ask the P+DT, research 04 section 5).
- **Disc-only theme**: without the P+ SD card there is no background gradient, so frames stay untinted grey on the neutral palette. Fine as a fallback, but P+ users always have the SD card.
- **MUI glyph icons** remain (see section 5).
- **Hard-coded P+ stock id table** in `catalog.ts`: a P+ release that renumbers characters needs it updated.
- **`--version` cost** and the update dialog (Dolphin fork issues 1-2).
- **Not verified on macOS, Linux or Steam Deck.** Paths for those are written (`dolphin/install/paths.ts`, DolphinTool lookup) but untested; the extractor itself is plain Node.
- **Network diagnostics** still use Google's public STUN servers.
- **Replays**: see section 6.
