# C ENet interop probe

`slippi_mm_probe.c` talks to the mm server through the C ENet library that Slippi's Dolphin bundles (`refs/slippi-Ishiiruka/Externals/enet`, 1.3.x), with the same host settings and message flow as `SlippiMatchmaking.cpp`. The Rust server and `mmclient` use `rusty_enet` (a Rust translation of ENet 1.3.18), so this checks that the two ENet implementations interoperate and that the JSON a C++ client receives is what it expects.

It is a manual check, not part of `cargo test`, because it needs a C compiler and the Slippi source tree.

Build (any of `zig cc`, `clang` or `gcc`; on Linux and macOS use `unix.c` instead of `win32.c` and drop the Windows libraries):

```sh
E=../../../refs/slippi-Ishiiruka/Externals/enet
zig cc -O1 -o probe slippi_mm_probe.c $E/{callbacks,compress,host,list,packet,peer,protocol,win32}.c -I$E/include -lws2_32 -lwinmm
```

Run two probes that name each other (uid and play key from two `user.json` files):

```sh
./probe 127.0.0.1 43113 <uid A> <playKey A> <code B> &
./probe 127.0.0.1 43113 <uid B> <playKey B> <code A>
```

Each prints `RECV {"type":"create-ticket-resp"}` and then the `get-ticket-resp` with the match, and exits 0. Result on 2026-10-06 (Windows 11, zig 0.12 build): both probes matched; a wrong play key got `create-ticket-resp` with the error and a disconnect.
