# brawlonline.net

P+ Rollback + servers.

https://brawlonline.net

# Credits

Brawl Online started as a fork of [Brawlback](https://github.com/Brawlback-Team), the rollback
project for Brawl and its mods. Thank you to everyone who built it:

- [WhiteTPoison](https://github.com/WhiteTPoison): Brawlback's lead, most of its game code and
  shared code, and its Project+ Dolphin
- [PiNE](https://github.com/FaultyPine): the first Brawl rollback prototype (2021) and much of
  Brawlback's Dolphin code
- [Kapedani](https://github.com/ilazoja)
- [barzamin](https://github.com/barzamin)
- [Believer1](https://github.com/Believer1)
- [blasphemetheus](https://github.com/blasphemetheus)
- [eliasreid](https://github.com/eliasreid)
- [pdeblois](https://github.com/pdeblois)
- [rapito](https://github.com/rapito)
- [WHellhaus](https://github.com/WHellhaus)

Also built on:

- [Nyx](https://github.com/NyxTheShield)'s fork of Brawlback's Project+ Dolphin (its rollback work,
  and the NAND journal we ported, which came from Orca by YouGame)
- [Syriinge](https://github.com/Sammi-Husky/Syriinge) by Sammi Husky, which Brawlback's game code
  is based on
- [GekkoNet](https://github.com/HeatXD/GekkoNet) by HeatXD
- [Slippi Launcher](https://github.com/project-slippi/slippi-launcher) by Project Slippi: our
  launcher is a fork of it
- [Dolphin](https://github.com/dolphin-emu/dolphin)
- [Project+](https://projectplusgame.com) by the Project+ Development Team

The licenses and what came from where are in [NOTICE](NOTICE) and the NOTICE files it lists.

# Building

```sh
git submodule update --init --recursive

# Dolphin (Windows, in a VS 2022 Developer PowerShell)
cd dolphin
cmake --preset ninja-release-x64
cmake --build --preset ninja-build-release-x64
cd ..

# Game plugin (needs GNU make and sh)
python tools/gamecode/setup_toolchain.py
cd game-code
./build.sh   # make
cd ..

# Launcher
cd launcher
npm ci
npm run stage:plugin
npm run package
cd ..

# Server
cd server
cargo build --release
```
