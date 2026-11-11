# Theme Color

Settings > Appearance > Theme Color: the player picks the launcher's colour on a colour wheel. Asked for on Sarah's board, 2026-11-11.

## Rules

- **Default.** With no colour chosen, nothing changes. The launcher is grey without the disc. With the disc and P+'s card it uses P+'s colours, which `styles/game_theme.tsx` samples from P+'s menu gradient texture (`menu2/sc_selmap.pac` bg_gradient: top #209a7a, middle #092f2a, bottom #205e84).
- **Chosen colour.** It is used with or without the disc and is kept when the disc is loaded later. Without the disc only the colours change. With the disc, the game's frames (Play buttons, dialogs) are tinted with the chosen colour instead of the gradient, and the game fonts and grid tile stay.
- **Setting.** `themeColor` is `"#rrggbb"`, or `null` for the default. Any other value counts as `null`.
- **Settings row.** The row shows a swatch of the current colour with a pencil. With no colour chosen, the swatch shows the default look's colour (grey, or P+'s teal). "Reset to default" sits to its left, and only while a colour is chosen.
  - Inline controls (this swatch and the App Language dropdown) end where the switches' thumbs end (`SettingItem inline`, `SWITCH_RIGHT_INSET`).
- **Picker.** The pencil takes the player to Home with a wheel in the middle of the window. Cancel is on the left under the wheel and Apply on the right.
  - The launcher shows each colour as it is picked, at most once a frame. The setting changes only on Apply.
  - Cancel, Escape, or leaving the main page (a shortcut) restores the previous look and returns to Settings > Appearance.
  - The page behind is not dimmed, but it does not take clicks.

## The colour math (`styles/theme_color.ts`)

All of it is done in OKLCH (perceived lightness, chroma and hue), so every hue comes out equally readable. P+'s gradient is the model:

| P+ | OKLCH L | Theme colour |
|---|---|---|
| top #209a7a: frame tint and accent | 0.614 | the chosen colour; accents at least L 0.60 |
| middle #092f2a: page | 0.277 | L 0.277, 38 % of the colour's chroma (at most 0.05); paper L 0.245 |
| bottom #205e84: second accent, dialog frames | 0.461 | L − 0.15, 77 % chroma, **same hue**; as an accent L 0.53–0.58 |

- **Same hue for the second colour.** P+'s blue is 70° from its teal. Even a 20° turn takes red to orange and orange to khaki (screenshots, 2026-11-11), so a theme keeps to one hue.
- **The wheel.** Hue goes around the circle. The distance from the centre is the share of that hue's most sRGB chroma at the current lightness, so every point is a usable colour and the centre is grey. The brightness slider runs from L 0.40 to 0.68.
  - Above 0.68, the light labels on the tinted frames get hard to read (yellow at 0.75 was).
  - Below 0.40, the frames are too dark for their black rim.
  - The plain look's #d0d0d0 is above the range, so the wheel starts in the grey centre at L 0.61.
- **Test.** `theme_color.test.ts` checks every wheel colour (24 hues × centre/mid/rim × min/mid/max lightness) against these contrast ratios:
  - accent on the page and on paper: at least 3:1
  - second colour on the page: at least 2:1
  - light text on the second colour: at least 2.5:1
  - body text: at least 10:1

## Colours that don't follow the theme (audit, 2026-11-11)

These carry meaning and stay fixed. A chosen colour can match one of them; nothing gets harder to read, but the meaning is less distinct.

- **Online / OK green.** #74c27a (friend online dot, room "waiting", ruleset OK, counterpick stages), #8fd394, rating-up #66bb6a. These clash with green themes.
- **Error / live red.** `--red-error` rgb(203 15 5), #f28b82, #ef5350 (rating down), spectate's `--live-red` #e5534b. These clash with red themes.
- **Warning / unread amber.** #e8b45a, #f2b33d, `--room-in-game` #d9a441. Unread chat is `--unread` 217 164 65; `social.module.css` says it was picked as the opposite of a purple accent. These clash with yellow, orange and brown themes.
- **Gold highlights.** The profile and create-room glows, stars, the winner crown, hazards and best-stat cells. These also clash with yellow themes.
- **Port and team colours** (`lib/player_colors.ts`): P1 #f15959, P2 #6565fe, P3 #febe3f, P4 #4ce44c. Match history's team dots use a second palette (`TEAM_COLORS` #ef5350/#42a5f5/#66bb6a). In a red theme the replay rows' P1 pill sits next to red action icons but is still clear (tmp/theme-color/sheet-replays.png).
- **Rank tier text** and the rank emblem PNGs.
- **Codes purple** (#c9a6f2, #b98ce0) and the own chat bubble. Slippi's leftover purple borders `rgb(159 116 192 / 10–28 %)` (content blocks, the chat window, the online search box, replay rows) are decoration. These are the one group that could reasonably follow the accent instead.
- **Neutrals that stay neutral:** `--surface-gray` (#222, chat window and tabs), the footer #000, the black search boxes (spectate, highlights, replay filter), the logo text `--off-white`, and the icon greys.

The accent also stands for relative states, which is fine because each compares against the rest of the same theme: selected folder or tab, winner glow, host crown, playing highlight, done ticks.

`styles/has_border.ts` writes `var(--accent-primary)` into an SVG data URI for the disc drop zone's drag border (`iso_selection_step.tsx`). A CSS variable can't resolve there, so that border probably never drew. This is not related to theming and was not changed.
