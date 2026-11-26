# Launcher changelogs ("patch notes")

Sarah's board, 2026-11-25: "i wonder if we can implement a changelog system for patches from now on. like, before i push, i set up a changelog to go out with the launcher ... a one-time popup on the launcher after update, they x it out and don't see it again. changelog buttons can come later. we can store a log of past changelogs on the server, but we don't need to create the ui to serve the history yet, just the latest one on update." The first version had a web editor on brawlonline.net; she then asked for it in the agent board instead: "copy the code for the markdown editor into the agent board ... the editing, the copy-paste of images / file upload, and the preview split ... it saves my changelog persistently even when i reload the tab or move to other machines / browsers. any media copy-pasted in / uploaded stays local to my agent board server machine. when i push to main from the kanban board, the changelog goes with it. there's also a checkbox that says "show on staging" that's on by default and means every staging run shows that popup with the current changelog cache every single time."

## Writing one (the agent board)

**Patch notes** in the board's header (`D:\code\agent-board`, `lib/patchnotes.js`) opens the editor: Markdown on the left, the launcher's popup on the right.

- The draft is kept on the board's server (`data/patch-notes/draft.json`) as it is typed, so a reload, another browser or another device on the tailnet shows the same draft. A save over a newer version from another tab is refused and offers "Load that version / Keep mine".
- Pictures and videos: paste, drop, or **Upload image or video**. They stay on the board's PC (`data/patch-notes/media/`) under the name accounts gives uploads (the first 20 hex of the content's SHA-256 and the type's extension), and the Markdown points at `/blog/media/<name>`, the address they will have on brawlonline.net. The board serves that address itself, so the Markdown is the same on both. PNG/JPEG/BMP screenshots become WebP when that is smaller (at most 2400 px wide), as in the blog editor. At most 95 MB per file.
- The preview is accounts' own renderer (`pp-admin changelog render`, the staging build's admin tool or the checkout's), in a box like the launcher's popup. Before a staging build has this tool, the preview says to build staging.
- The Markdown is the blog's (docs/design/blog.md §2): a `#` heading is the popup's big heading, an image alone in its paragraph is a figure with its `[caption]`, `.mp4`/`.webm`/`.mov` are videos, raw HTML shows as text.

## Shipping it (Push to main)

**Push to main** (in the board's Changelog drawer) first publishes the patch notes on prod: `ssh debian@sarahvps2 sudo pp-admin changelog publish`, with the Markdown and every picture it shows as JSON on stdin. pp-admin writes the pictures into the blog's media folder (`BLOG_MEDIA_DIR`, `/var/lib/ppserver/blog-media`, mode 0644 so accounts reads them) and adds a `changelogs` row. Then it pushes. If publishing fails, nothing is pushed. If the push fails after publishing, pushing again doesn't publish the same text twice. After a push that carried them, the draft is kept in `data/patch-notes/history/` and the editor starts empty. An empty draft publishes nothing: that release has no popup. The push's confirm says which.

Publishing before the push matters: a launcher shows a changelog only if it was published before that launcher was built (below), and the release is built after the push.

## Staging ("Show on staging", on by default)

Each staging run from the board (▶ Windows, ▶ Mac, Run 2v2) first runs `staging.py admin changelog publish --every-start` with the draft (or `clear-every-start` with the box off or the draft empty). That puts it on the staging server as an `every_start` changelog, which replaces the last one and comes ahead of the others, and every test-mode launcher (staging's) shows it at every start, whatever it showed before. This step never fails the run; what went wrong is in the run's log. Prod never has an every-start changelog.

## What players see

After an update the launcher's main screen opens the newest changelog in a popup over the page. The X (or Esc) closes it for good; a click beside it doesn't close it (a click in the launcher as it opens would lose it). Links open in the browser.

Exactly when:

- At each start the main process compares its version with the one that ran last (`lastRunVersion` in Settings, `main/changelog_version.ts`). A different one is an update and sets `changelogPending`. The first start of the release that brings changelogs has no `lastRunVersion`: it counts as an update unless the launcher has no settings at all (a new install, which shows nothing).
- With `changelogPending`, the page (`lib/changelog/changelog_dialog.tsx`, mounted with the main screen, so after setup or login) asks `GET /v1/changelogs/latest` once. It opens the popup when that changelog was published after the last one this launcher showed (`changelogSeenAt`) and before this launcher was built (`__DATE__`); otherwise it clears `changelogPending`. Closing the popup clears it and stores the changelog's publish time in `changelogSeenAt`. Offline or a failed request: it asks again at the next start.
- A test-mode launcher asks at every start, and an `everyStart` changelog opens every time without being stored as seen.
- So a launcher that skipped releases sees the newest changelog only, and one published for a release still building doesn't show on the release before it.

**Settings > Changelog** (above Help; Sarah's board, 2026-11-26: "in settings, above "help", there should be "changelog". clicking it doesn't open a new page, it just makes the latest patch changelog popup appear.") asks `GET /v1/changelogs/latest` when clicked and opens the same popup over Settings (`lib/changelog/latest_changelog.tsx`, mounted at the root in `app/create.tsx`, as the startup one lives with the main screen). It shows whatever is newest, without the build-time check, and closing it stores nothing, so the popup after the next update still comes. None published is a "No changelog yet." toast; a failed request is the usual error toast. Settings' entries can be such actions (`SettingAction` in `pages/settings/types.ts`): a button, no page or route.

The page cleans the server's HTML once more (`changelog_html.ts`): only the elements and attributes the blog's renderer writes, pictures and videos only from the accounts server, links absolute and opened outside.

## Server

- `changelogs` (migration 0046): `body_md`, `published_at`, `every_start`. Every changelog stays as the history (`pp-admin changelog list`); nothing serves the older ones yet (Settings > Changelog shows the newest).
- `GET /v1/changelogs/latest` (public, `max-age=60`): `{changelog: {id, html, publishedAt, everyStart} | null}`, the every-start one if there is one, else the newest, rendered by `blog::render`.
- `pp-admin changelog publish [--every-start] | clear-every-start | list | render` (`server/crates/admin/src/changelog.rs`, `accounts::changelog::publish`). `publish` refuses a picture that is neither sent nor already in the media folder, and anything that isn't a PNG, JPEG, GIF, WebP, AVIF, MP4, WebM or MOV (by its bytes). Audited as `changelog.publish` / `changelog.publish_every_start`.
- Tests: `changelog::tests`, `e2e --test changelogs`; launcher `main/changelog_version.test.ts`, `lib/changelog/changelog_html.test.ts`.

## The first push

The board publishes with prod's pp-admin, which learns `changelog` from the push that brings this code. So for that push the board publishes after it: when publishing before the push fails because prod's pp-admin doesn't know `changelog` (or the table isn't there yet), it notes the time a minute before the push, pushes, then tries again every 20 s for up to 45 minutes (the server deploy takes about 8) with `pp-admin changelog publish --published-at <that time>`. Every launcher that push builds was built after that time, so it counts the notes as published before it was built. A launcher that updates and starts before they are published finds none and asks again at its next start (`latestChangelog` answering null leaves `changelogPending`). If the 45 minutes pass, the push is done but the board says the notes are not published, with the command to publish them by hand. From the push after it on, publishing goes before the push as above. `--published-at` takes a time at most 2 days back and never ahead.
