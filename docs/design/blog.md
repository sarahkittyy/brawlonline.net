# Blog (brawlonline.net/blog)

Sarah's board, 2026-11-12: "i'd like to create a blog ... on the brawlonline net homepage, create a `blog` button to the right of `credits` ... easy for me to add and remove articles at a whim. markdown is probably fine, but i'll need to support images too ... ctrl-v them into the blog as i'm writing it". From the mockups she picked the index with **the latest post up top and older posts below** and the split-pane editor (without the squashed logo). Her other picks: posts in the database with a web editor; videos too, with a file upload button; no RSS feed; and a **generic login** (a site-wide session, not part of the blog), so a later `/admin` page can use the same login.

## 1. Pages

| Address | What | Served by |
| --- | --- | --- |
| `/blog` | The index: the newest post as a big card (its first image as the cover, else the home page's gameplay still; title, date, summary, **Read**), the others as rows under "Older posts" | accounts (`blog.rs`) |
| `/blog/<slug>` | A post: title, date · by &lt;author&gt; · N min read, the text. A draft only with its preview link (`?preview=<token>`, `noindex`, a yellow "Draft preview" line) | accounts |
| `/blog/media/<hash>.<ext>` | An uploaded image or video (byte ranges, cached for good: the name is the content's hash) | accounts |
| `/blog/blog.css` | The blog's style (the editor's preview loads it too) | static (`website/blog/`) |
| `/blog/write/` | The editor (admins only) | static (`website/blog/write/`) |
| `/login/` | The website's login, `?next=<path>` to come back | static (`website/login/`) |
| `/auth/auth.js`, `/auth/auth.css` | The site's session and the logged-in pages' look | static (`website/auth/`) |

The home page has a **Blog** button right of Credits. The blog pages have the same three buttons top left (Blog lit), a small logo plate with "BLOG" beside it, the dim gameplay still behind, and the dark cards with the green top edge from the FAQ and the Top 10. They have no scripts. Their CSP allows only the site itself, so a post can't load anything from elsewhere.

Search engines and link previews (all server-rendered like `/profile/<code>`, so crawlers get the whole page without running anything):

- Each page has a `<title>`, a meta description (the summary), an absolute canonical URL (`PUBLIC_BASE_URL`, `https://brawlonline.net` on prod) and exactly one `<h1>`: the post's title, or the logo and "Blog" on the index. A `#` heading in a post's text becomes an h2.
- Link previews: `og:title`, `og:description`, `og:image` (the first image) and `article:published_time`.
- JSON-LD: a `BlogPosting` on each post (headline, dates, image, author when set, publisher) and a `Blog` listing the posts on the index.
- `/blog/sitemap.xml` (accounts) lists `/blog` and every published post with its last change, and robots.txt names it next to the static `/sitemap.xml`. Discovery also works by links: home → Blog → each post.
- A draft's preview link, the 404 page and the error page are `noindex`. A missing post answers 404, not a 200. `/blog/` redirects to `/blog` with a 301.
- `/login/` and `/blog/write/` are disallowed in robots.txt and `noindex`.
- After the first deploy: in Google Search Console (the domain is already verified there), submit `https://brawlonline.net/blog/sitemap.xml` and inspect the post's URL ("Request indexing") to have it crawled sooner than on its own.
- Not done: images have no width/height (a little layout shift while they load; the server doesn't know an upload's dimensions).

## 2. Posts

A row of `blog_posts` (migration 0033): `slug` (the address; lowercase letters, digits, single dashes; `write` and `media` are taken), `title`, `summary` (empty: the first paragraph, cut near 200 characters), `author` (the byline; empty: none), `body_md`, `status` (`draft`/`published`), `published_at` (set by the first publish and kept through unpublish/publish, the date the pages show), `preview_token`.

Markdown is CommonMark with GitHub's tables, strikethrough and task lists, plus footnotes (pulldown-cmark), so text pasted from a gist reads the same. Our own rules (`blog::render`, also the editor's preview through `POST /v1/blog/render`):

- An image alone in its paragraph is a figure, and its alt text is the caption shown under it: `![The new waiting room](/blog/media/….webp)`. Write `![](…)` for no caption.
- An image whose address ends in `.mp4`, `.webm` or `.mov` is a video with controls (`preload="metadata"`, so nothing big loads until it plays).
- An image from another site becomes a link to it, because the pages load nothing from elsewhere. Upload it instead.
- Raw HTML shows as text. `<br>` is kept, and HTML comments (`<!-- notes to self -->`) are dropped.
- Links to `javascript:`, `data:` and the like are shown as plain text, not links.
- `##` and `###` headings get ids (`#new-ways-to-play-project-together`) for linking to a section.

## 3. Writing (the editor)

1. Go to `brawlonline.net/blog/write/`. Without a session it sends you to `/login/` and back. Log in with your launcher account; the account needs `role = 'admin'` (§6).
2. **New post** makes an empty draft. Pasting a whole Markdown post into it (like the gist) takes its `# Title` line as the title. The address follows the title until you edit it or the post is first published.
3. Write on the left; the right side is the page as it will look (the same renderer and stylesheet). It scrolls with the Markdown block by block: `POST /v1/blog/render` also returns the source line of each top-level element (`blocks`), the editor measures those lines in a copy of the textarea, and the block at the top of the Markdown is at the top of the preview, however tall its pictures and videos are. Each render keeps the preview's unchanged elements, so a playing video keeps playing while you type and pictures don't load again.
4. **Images and videos:** Ctrl+V a screenshot, drop files on the text, or click **Upload image or video** (a file picker, several at once). Each one goes up right away. An "Uploading…" note marks its place (a comment, so the preview ignores it) and becomes `![](/blog/media/<hash>.webp)` when the upload is done; a progress bar with Cancel sits bottom left. PNG/JPEG/BMP are re-encoded as WebP (quality 0.9, at most 2400 px wide) in the browser when that is smaller; GIF, WebP, AVIF and videos go as they are. One file is at most 95 MB (Cloudflare refuses requests over 100 MB), so shorten or compress a long clip first. The server types each file by its first bytes, not by its name: PNG, JPEG, GIF, WebP, AVIF, MP4, MOV, WebM. Anything else (SVG included) is refused.
5. **Saving:** a draft saves itself 1.5 s after you stop typing. A published post's changes wait for **Update** (or Ctrl+S), so half-written sentences never go live. Until the server has a change it is also kept in the browser (localStorage, also written as the tab closes), the list marks that post "Unsaved changes", and reopening it offers them back. A save over a newer version (the post changed in another tab) is refused with "Load that version / Keep mine" rather than overwriting it.
6. **Editing a published post:** open it from the list, or go straight to it with `/blog/write/#<address>` (e.g. `/blog/write/#progress-report-1`). It's the same editor. Changes wait for **Update**, which puts them on the same post: same link, same id, same publish date. A published post's address is locked, because links people shared and search results point at it. **change** next to it unlocks it after a warning; the old link then shows "No such post".
7. **Preview link** (drafts): anyone with the link can read the draft, to show someone before publishing. **Publish** puts it on the blog (it asks first). **Unpublish** makes it a draft again; **Delete** removes it (its uploads stay, another post may use them).

Byline: the **By** field is free text (the progress report is AlephZeroChill's), and empty means no byline.

## 4. The website's login (generic)

`website/auth/auth.js` is the one session for every logged-in page of the site. It is the launcher's account system: `/login/` posts to `/v1/auth/login` (a session of its own; the launcher's sessions are untouched) and keeps the token in localStorage. Pages send it as `Authorization: Bearer <token>`, as the launcher does. No cookies, so no CSRF to guard against; script-src 'self' on every page keeps other scripts away from the token. Nothing on the home page links to `/login/`.

A future `/admin/` page needs only:

```html
<link rel="stylesheet" href="/auth/auth.css">
<script src="/auth/auth.js" defer></script>
```
```js
const session = await BrawlAuth.requireLogin();          // to /login/?next=<here> and back without one
if (!BrawlAuth.hasRole(session, "admin")) { /* say so */ }
const data = await BrawlAuth.api("/v1/admin/...", { method: "POST", json: {...} });  // ApiError on failure
await BrawlAuth.upload("/v1/...", file, { onProgress });  // raw-body upload with progress
BrawlAuth.mountAccount(element);                          // "Name CODE [Log out]"
```

`requireLogin` re-reads `/v1/me` each time, so a changed role counts at once, and a 401 anywhere drops the session. Logging out in one tab logs out the others (the `storage` event). On the server, an admin-only endpoint calls `current_user` and checks `user.role == "admin"` (as `blog::require_admin`). `auth.css` has the shared pieces: the one-line BRAWL ONLINE mark, the green plate button (`.btn`), pills, fields, cards, the account chip. nginx (and the staging accounts with `WEBSITE_DIR`) serve `/auth/` and `/login/` as static files.

## 5. Server

- Editor API (`/v1/blog/...`, Bearer session, 403 `not_admin` for everyone but admins): `GET/POST posts`, `GET/PUT/DELETE posts/<id>` (PUT takes `expectedUpdatedAt`, 409 `changed_elsewhere`; 409 `slug_taken`, 400 `bad_slug`), `POST posts/<id>/publish|unpublish`, `POST render`, `PUT media` (raw body, `X-File-Name`). Post bodies up to 1 MiB (their own body limit; the API's 16 KiB is for the rest).
- Media: streamed to a `.partial` in `BLOG_MEDIA_DIR` while hashed, typed, then renamed to `<20 hex of SHA-256>.<ext>` (the same file uploaded twice is stored once) and recorded in `blog_media` (kind, bytes, original name, uploader). Limits: `BLOG_MEDIA_MAX_BYTES` (95 MiB per file), `BLOG_MEDIA_MAX_TOTAL_BYTES` (20 GiB; 507 past it). Served by accounts (tower-http `ServeFile`: ranges for video seeking, `Cache-Control: immutable`). They are **not in the database backups** (`pp-backup.sh` dumps the posts only).
- Pages: `public, max-age=60` (a preview link: `no-store`), 120 per minute per IP (`BLOG_IP_WINDOWS`).

## 6. One-time production setup

A push deploys accounts (the migration runs on start) and the website, but not the nginx site or the systemd unit. Without those two steps, `/blog` is nginx's 404 and uploads fail. On `debian@sarahvps2`:

1. **nginx:** the `^~ /v1/blog/` location (100m bodies, streamed) and the `/blog` locations, in `server/deploy/sarahvps2/nginx-brawl-online.conf`. Copy the same edit into `/etc/nginx/sites-available/brawl-online.conf`. Prod's copy differs from the repo's, so add the blocks instead of copying the file. Back it up to `/etc/nginx/brawl-online.conf.bak-<utc>`, run `sudo nginx -t`, then `sudo systemctl reload nginx` (never restart). Safe before the accounts change ships: until then `/blog` answers accounts' 404.
2. **The unit:** `StateDirectory=ppserver/bug-reports ppserver/blog-media` in `/etc/systemd/system/pp-accounts.service` (as `server/deploy/sarahvps2/pp-accounts.service`), then `sudo systemctl daemon-reload`. It takes effect at the next accounts restart (every deploy restarts it). Without it, uploads answer 503 `storage_unavailable` (`ProtectSystem=strict`).
3. **Your account:** `sudo pp-admin user role <your email> admin` (audited as `user.role`).
4. Push. Then write the post at `/blog/write/`.

## 7. Trying it locally

accounts with `WEBSITE_DIR=<repo>/website` serves the static pages itself (`/blog/blog.css`, `/blog/write/`, `/login/`, `/auth/`), so staging (`tools/staging/staging.py`) or a bare `accounts` works without nginx. Make an account admin with `admin user role`. Tests: `blog::tests` (renderer, slugs, sniffing, escaping) and `e2e --test blog` (admin-only API, drafts and preview links, publish/unpublish, the index's hero and older posts, uploads, ranges, the 409s).
