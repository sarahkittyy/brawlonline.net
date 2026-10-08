# brawlonline.net

P+ Rollback + servers.

https://brawlonline.net

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
