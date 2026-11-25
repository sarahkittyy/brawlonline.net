# Launcher changelogs

Sarah's board, 2026-11-25: "i wonder if we can implement a changelog system for patches from now on. like, before i push, i set up a changelog to go out with the launcher." Written in Markdown with images, in the blog editor "shrunk to remove all the title features"; "a one-time popup on the launcher after update, they x it out and don't see it again. changelog buttons can come later. we can store a log of past changelogs on the server, but we don't need to create the ui to serve the history yet, just the latest one on update."

## Writing one

1. `brawlonline.net/changelog/write/` (admins only, the website's login: same account and `role = 'admin'` as the blog editor, docs/design/blog.md §4). It is the blog editor (the same `website/blog/write/editor.js`, `<body data-kind="changelog">`) without the title, address, byline and summary: only the text. Images and videos as in the blog (paste, drop, **Upload image or video**; they go to `/v1/blog/media` and live with the blog's). The preview is the server's renderer in a box like the launcher's popup (`website/changelog/write/changelog.css`, the launcher's default theme; the launcher's own theme colours and fonts differ a little).
2. Drafts save themselves; a published one's changes wait for **Update** (Ctrl+S).
3. **Publish before you push.** A launcher shows a changelog only if it was published before that launcher was built, so publish it, then push; the release built after the push carries it. One published after the release was built waits for the next release.

The editor's list shows each changelog by its first line (write a `# Heading` first). Markdown is the blog's (docs/design/blog.md §2): a `#` heading shows as the popup's big heading, an image alone in its paragraph is a figure with its `[caption]`, `.mp4`/`.webm`/`.mov` are videos, raw HTML shows as text.

## What players see

After an update the launcher's main screen opens the newest published changelog in a popup over the page. The X (or Esc) closes it for good; a click beside it doesn't close it (a click in the launcher as it opens would lose it). Links open in the browser.

Exactly when:

- At each start the main process compares its version with the one that ran last (`lastRunVersion` in Settings, `main/changelog_version.ts`). A different one is an update and sets `changelogPending`. The first start of the release that brings changelogs has no `lastRunVersion`: it counts as an update unless the launcher has no settings at all (a new install, which shows nothing).
- With `changelogPending`, the page (`lib/changelog/changelog_dialog.tsx`, mounted with the main screen, so after setup or login) asks `GET /v1/changelogs/latest` once. It opens the popup when that changelog was published after the last one this launcher showed (`changelogSeenAt`) and before this launcher was built (`__DATE__`); otherwise it clears `changelogPending`. Closing the popup clears it and stores the changelog's publish time in `changelogSeenAt`. Offline or a failed request: it asks again at the next start.
- So a fixed typo in a published changelog never shows it twice (same publish time), a launcher that skipped releases sees the newest changelog only, and a changelog published for a release still building doesn't show on the release before it.

The page cleans the server's HTML once more (`changelog_html.ts`): only the elements and attributes the blog's renderer writes, pictures and videos only from the accounts server, links absolute and opened outside.

## Server

- `changelogs` (migration 0046): `body_md`, `status` (`draft`/`published`), `published_at` (the first publish's, kept through unpublish/publish), `created_by`, `created_at`, `updated_at`. Every changelog stays as the history; nothing serves the older ones yet.
- `GET /v1/changelogs/latest` (public, `max-age=60`): `{changelog: {id, html, publishedAt} | null}`, the newest published one rendered by `blog::render`.
- Editor API (Bearer session, 403 `not_admin` for others, as `admin::require_admin`): `GET/POST /v1/changelogs`, `GET/PUT/DELETE /v1/changelogs/<id>` (PUT `{body, expectedUpdatedAt}`, 409 `changed_elsewhere`), `POST /v1/changelogs/<id>/publish|unpublish` (400 `empty` for an empty one). A changelog is at most 48 KB of text (413 `too_large`), so its requests stay under prod nginx's 64 KB body cap: no nginx change.
- Tests: `changelog::tests`, `e2e --test changelogs`; launcher `main/changelog_version.test.ts`, `lib/changelog/changelog_html.test.ts`.

## Shipping it

One push deploys accounts (the migration), the website (the editor) and a launcher release with the popup. The editor works from that deploy on; that release's own launchers show the first changelog published before it was built, so the first one to reach players is the one written before the push after that (or, to have one in the very first release, deploy the server and website alone first).
