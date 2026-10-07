# PlusOnline Launcher

Desktop launcher for Project+ online play with rollback netcode: log in, get a connect code, press Play to start our Dolphin build with Project+, browse replays, and change settings.

"PlusOnline" is a placeholder name. The product name lives in one constant (`src/common/product.ts`); the service hosts are configuration there too (subdomains of `fluffycat.gay` by default).

This is a fork of the [Slippi Launcher](https://github.com/project-slippi/slippi-launcher) by Project Slippi, with its git history kept. See [NOTICE](NOTICE) for attribution and [PPLUS_PORTING.md](PPLUS_PORTING.md) for what changed and why.

## Game assets are never bundled

The launcher's look comes from the user's own copy of Super Smash Bros. Brawl and Project+ SD card. At first run, after a disc is chosen, it extracts menu frames, fonts and stock icons into a local cache (`src/brawl_assets`, `src/game_assets`). Until then it shows a plain, unstyled fallback. Nothing from the game is committed or distributed; `.asset-cache/` is gitignored.

## Development

Needs Node 20+ and Git.

```sh
npm ci
npm run build          # main, renderer and migrations (needed before the tests)
npm run typecheck
npm run lint
npm test
npm start              # run the app in development mode
```

`npm run dev` runs the renderer with mocked services (log in as `test` / `test`).

Development defaults (all overridable, see `.env.example`):

| What | Default in development | Override |
|---|---|---|
| Accounts API | `https://accounts.fluffycat.gay` | `PPO_ACCOUNTS_URL` (e.g. `http://127.0.0.1:8080` for `server/`) |
| Dolphin executable | `../dolphin/build/release/x64/Binaries/Dolphin.exe` | Settings > Dolphin, or `PPO_DOLPHIN_PATH` |
| Dolphin User folder seed (P+ launcher DOLs, `Wii/sd.raw`) | `../run/template-user` | `PPO_DOLPHIN_USER_TEMPLATE` |
| Disc files for asset extraction | `../game/rev1-extract/DATA/files` if present, else the chosen ISO via DolphinTool | `PPO_DISC_FOLDER` |
| Asset cache | `./.asset-cache` | `PPO_ASSET_CACHE` |

The asset extractor also has a CLI:

```sh
npx ts-node --transpile-only src/brawl_assets/cli.ts --disc-folder <DATA/files> --sd <sd.raw> --out .asset-cache
```

## License

GPL-3.0, like the Slippi Launcher. See [LICENSE](LICENSE).
