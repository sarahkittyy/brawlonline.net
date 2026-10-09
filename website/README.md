# brawlonline.net

The landing page for Brawl Online. It is a static site with no build step, no analytics, no cookies and no external requests (a Content-Security-Policy meta tag in `index.html` enforces `'self'` only). The page shows a logo with the tagline "rollback + matchmaking for pm" under its bottom right corner, one download button for the visitor's OS, a tiny "download for other operating systems" toggle, and a dim looping gameplay video behind them. The logo and the button sit together in the middle of the screen, both on green menu-button frames in the style of Project M's menus. There is no other text, by design (the tagline is `.tagline` in `index.html` and `style.css`).

| File | What it is |
| --- | --- |
| `index.html` | The page. Icons are inline SVG. |
| `style.css` | All styles, including the menu-button frame (`.plate`). `--video-opacity` sets how dim the video is; the `--pm-*` variables set the frame colours. |
| `script.js` | The download links (`DOWNLOADS` at the top), OS detection and video start. |
| `assets/fonts/` | The wordmark font (Barlow Condensed Black Italic, OFL) and its licence. |
| `assets/bg.webm`, `assets/bg.mp4`, `assets/bg-poster.jpg` | Background video and its still frame. **Not present yet.** |
| `LICENSES.txt` | Credits for the font, the icons and (later) the video. Deploy it with the site. |

Without the video files the page is plain dark and still looks right. The browser logs 404s for the three missing files until they exist.

Preview locally:

```sh
cd website
python -m http.server 8765    # then open http://localhost:8765
```

## The logo and the menu-button frames

The logo is a two-line wordmark: "BRAWL" at the top left and "ONLINE" offset to the bottom right, in white with a heavy dark outline, on a green menu-button frame. The download button uses the same frame. Both frames are one CSS rule, `.plate::before` in `style.css`: a slanted (`skewX`) rounded plate with a dark outer rim, a thin light-green inner border, a green top-to-bottom gradient face, a glossy upper half, a lit top edge and a shaded bottom edge, plus a faint outer hairline so the dark rim reads on the dark page. It is sized in `em`, so it scales with the element's font size (`.logo.plate` and `.dl.plate`). The colours are the `--pm-*` variables at the top of `style.css`.

**This is a hand-built CSS recreation.** No game textures, screenshots or AI-generated graphics are in it or anywhere on the site; the frame was drawn from scratch by matching these references by eye:

- **Shape, rim and bevel:** Brawl's own menu textures as extracted by the launcher from the user's disc and SD card (`launcher/.asset-cache/survey/`, not part of the site): `MenCmn00` (the common menu button plate: rounded, thick black rim, bevelled face, in `frames_sd.png` / `mm_cand.png`) for the rim and bevel, and `MenMainFrMul01` / the `mm_disc_a.png` main-menu panels (rounded panels with a slanted side) for the slant. These textures are greyscale masks that the game tints at run time, so they give the shape only.
- **Colour:** Project M's green menu colour scheme (PM 3.6 main menu). No reference image of it was stored with the project; the greens were picked by eye to that scheme and are easy to retune in the `--pm-*` variables.
- **Lettering:** the white, heavily outlined mode labels on the P+ menus (`VERSUS`, `SOLO`, `CLASSIC` in `frames_sd.png`). Barlow Condensed **Black (900) Italic** is the closest free match: the menu labels are black-weight and condensed, and the italic leans the same way as the frame and the Smash series' logos.

### Swapping the logo for an image

To use a drawn logo instead of the wordmark:

1. Put the file at `assets/logo.svg` (or `assets/logo.png`; a PNG should be about 1200 px wide for sharp phones).
2. In `index.html`, inside `<h1 class="logo plate">`, delete the `<span class="wordmark">` line and uncomment the `<img>` line under it (change the extension if it is a PNG). Remove `plate` from the `<h1>`'s class if the image brings its own frame.

`style.css` already sizes `.logo img` (at most 560 px wide and 30% of the screen height). If nothing else uses the font any more, also delete the `<link rel="preload">` for it in `index.html`, the `@font-face` and `.wordmark` rules in `style.css`, `assets/fonts/`, and section 1 of `LICENSES.txt`.

No AI-generated graphics: the logo has to be made by a person, and it must not reuse Nintendo's or Project M/P+'s actual textures.

## Swapping the background video

Put the clip in `assets/` as `bg.webm` (VP9) and `bg.mp4` (H.264) and a still as `bg-poster.jpg`. It plays muted and looped behind a dark page at `opacity: 0.16`, so it can be low resolution and low bitrate. Aim for 10 to 20 seconds that loop cleanly and about 1 to 3 MB per file. With ffmpeg:

```sh
# Source: src.mp4; trim with -ss/-t as needed. Audio is dropped.
ffmpeg -i src.mp4 -an -vf "scale=-2:540,fps=30" -c:v libvpx-vp9 -b:v 0 -crf 42 -row-mt 1 assets/bg.webm
ffmpeg -i src.mp4 -an -vf "scale=-2:540,fps=30" -c:v libx264 -crf 30 -preset slow -pix_fmt yuv420p -movflags +faststart assets/bg.mp4
ffmpeg -i assets/bg.mp4 -frames:v 1 -q:v 5 assets/bg-poster.jpg
```

Then record the footage's source, author and licence in `LICENSES.txt` section 3 and in the credits comment in `index.html`. If the licence requires *visible* attribution (for example CC BY), add the smallest possible credit to the page (a single line of the `.more` size under the toggle is enough) and tell the user, since the page otherwise has no text.

Behaviour: the video starts from `script.js` only when the visitor does not ask for reduced motion (`prefers-reduced-motion: reduce`) and has not turned on data saving; otherwise the poster frame shows. `preload="none"` means nothing is fetched until it plays. To make it brighter or dimmer, change `--video-opacity` in `style.css` (0.12 to 0.2 keeps it in the background).

## Setting the download links

Edit `DOWNLOADS` at the top of `script.js`:

```js
const DOWNLOADS = {
  windows: "/downloads/BrawlOnline-Setup.exe",
  macos: "/downloads/BrawlOnline.dmg",
  linux: "/downloads/BrawlOnline.AppImage",
};
```

These are placeholders: there are no builds yet. Also update the three `href`s in the `<ul id="other">` list in `index.html`, which are the fallback for visitors without JavaScript.

The script picks the visitor's OS (Windows, macOS, Linux; the Steam Deck counts as Linux) and puts that OS's icon and link on the button. Phones, tablets (iPad included), ChromeOS and anything unrecognised get a neutral download icon that opens the list of all three. `window.BrawlOnline.detectOS({ ua, platform, touchPoints, uaData })` is exposed for testing the detection with any user agent.

The launcher's own update feed (electron-updater, `generic` provider) is `https://brawlonline.net/updates/launcher` (`launcher/src/common/product.ts`). The download links can point at the installers there once releases are published, or at a stable copy under `/downloads/`.

## Deploying

Copy the folder's contents (everything except `README.md`) to the web root that serves `https://brawlonline.net`. On the production box (`sarahvps2`, nginx) that is `/var/www/brawlonline`; the nginx site and its notes are in `server/deploy/`. nginx proxies only the accounts paths (`/v1/`, `/user/`, `/verify-email`, `/reset-password`, `/healthz`) and serves everything else from the web root, so `/downloads/` and `/updates/` live there too.

```sh
rsync -av --delete --exclude README.md \
  --exclude downloads/ --exclude updates/ \
  website/ debian@sarahvps2:/var/www/brawlonline/
```

Keep the excludes: without them `--delete` would remove the published installers and the update feed. The server needs MIME types for `.webm`, `.mp4` and `.woff2` (nginx's default `mime.types` has them).
