# ppharness: end-to-end harness for the P+ rollback Dolphin fork

`ppharness` is a Python package that drives our Dolphin fork through the harness control protocol (see [`docs/harness-protocol.md`](../docs/harness-protocol.md)). With it you can:

- boot isolated Dolphin instances;
- press buttons;
- read and hash memory;
- take screenshots;
- run two-player rollback netplay through a simulated network;
- check that both sides have the same state.

The core uses only the Python 3.12 standard library. It works on Windows today, and is written for Linux and macOS too: anything platform-specific lives in `ppharness/_platform.py`.

## Setup

```sh
# from the workspace root (D:\code\pm_rollback)
python -m venv .venv
.venv\Scripts\python -m pip install pytest      # Linux/macOS: .venv/bin/python
.venv\Scripts\python -m pip install -e harness  # optional; adds a `ppharness` command
```

You can also skip the install and run from `harness/`, e.g. `cd harness; ..\.venv\Scripts\python -m ppharness ...`.

What it expects (each path can be overridden with an environment variable):

| what | default | override |
|---|---|---|
| DolphinNoGUI | `dolphin/build/release/x64/Binaries/DolphinNoGUI.exe` | `PPHARNESS_DOLPHIN` (or `PPHARNESS_DOLPHIN_DIR`) |
| template user dir | `run/template-user` | `PPHARNESS_TEMPLATE` |
| instance dirs | `run/instances/<name>-<n>` | `PPHARNESS_INSTANCES` |
| keep instance dirs | off | `PPHARNESS_KEEP=1` |

To check whether the Dolphin build has harness support:

```sh
python -m ppharness probe        # {"harness": true, "detail": "protocol v1"}
```

## Sound is off by default

Every instance the harness launches is **muted**:

- `[DSP] Muted = True` is written into the instance's `Config/Dolphin.ini`;
- `-C Dolphin.DSP.Muted=True` is passed on the command line;
- the Dolphin server mutes harness sessions on its own as well (`PPR_HARNESS_AUDIO`).

`[Input] BackgroundInput = False` is always written too, and the physical bindings of all four GC pads are cleared, so your keyboard and controllers can't leak into a test.

To hear a run, opt in:

- in Python: `DolphinInstance(..., audio=True)` or `InstanceConfig(audio=True)`;
- on the command line: `--audio` on `boot`, `shot` or `netplay`.

That writes `Muted = False`, drops the `-C` flag and sets `PPR_HARNESS_AUDIO=1` for the process. (Muted is the default for now; we'll flip it later.)

## Commands (`python -m ppharness ...`)

| command | what it does |
|---|---|
| `boot [--wait-frames N] [--detach \| --hold [S]]` | Launches one instance, prints its status as JSON, then quits it and removes its dir. `--detach` leaves it running and prints its port. |
| `status --port P [--netplay]` | Status of a running instance. |
| `cmd --port P <command> key=value ...` | Sends any protocol command. Values are parsed as JSON or `0x` hex, e.g. `cmd --port P read_u32 addr=0x80000000`. |
| `shot PATH [--port P] [--frames N]` | Screenshot to a `.png`. Without `--port` it boots a fresh instance (headless D3D11 on Windows), waits N frames (default 1500, which reaches character select) and shoots. |
| `stop --port P` | Sends `quit`. |
| `netplay --preset typical [--no-rollback] [--duration S]` | Host and joiner through netsim. Prints status, rollback counters and netsim stats every `--interval` seconds. |
| `probe` | Does the build answer `ping` on `--harness-port`? |
| `netsim ...` | The UDP impairment proxy (same as `python -m ppharness.netsim`). |
| `bench [--preset P]` | Measures netsim accuracy (same as `python -m ppharness.netsim_bench`). |
| `clean [--dry-run]` | Deletes leftover instance dirs whose process is gone. Skips dirs another session still has files open in, and unlaunched dirs younger than 10 minutes. |
| `mock [--port P]` | Runs a mock harness server (fake memory and frames). |
| `scorecard [--presets lan,typical,bad_wifi] [--sessions N] [--duration S] [--phase menus\|match\|auto] [--json F]` | Rollback health benchmark: N two-instance rollback sessions per netsim preset with seeded random input; per side rollbacks, max depth, frames resimulated, desyncs, frames ahead over time, VI/game/GekkoNet FPS and freezes (>2 s without progress), plus a peer state comparison. Robust to a frozen side. JSON + text summary. |

Common instance flags:

- `--exe`, `--template`, `--video Null|D3D11|Vulkan|OGL`, `--platform headless`;
- `--cpu-thread on|off`, `--ports 0,1` (ports set to Standard Controller);
- `--audio`, `--keep`, `--name`;
- `--fake` (use the fake Dolphin, no build needed).

Typical interactive loop:

```sh
python -m ppharness boot --detach                   # prints {"port": 5xxxx, "user_dir": ...}
python -m ppharness cmd --port 5xxxx pad_set port=0 'buttons=["A"]'
python -m ppharness cmd --port 5xxxx wait_frame frame=2000
python -m ppharness shot out.png --port 5xxxx       # needs a non-Null video backend
python -m ppharness stop --port 5xxxx
python -m ppharness clean
```

## Python API

```python
from ppharness import DolphinInstance, InstanceConfig, PadInput
from ppharness.session import two_player_netplay, compare_state, FrameKey

# One instance: copies the template, launches with --harness-port, connects.
with DolphinInstance("smoke", config=InstanceConfig(cpu_thread=True)) as d:
    c = d.client                                    # HarnessClient
    c.wait_state("running")
    c.pad_script(0, [PadInput(buttons=["A"], hold=3), PadInput(hold=10)])
    c.wait_frame(input_polls=c.status().input_polls + 20)
    d.mark("pressed A")                             # log_mark + wait until it's in dolphin.log
    d.wait_for_log(r"some message", timeout=30)
    print(c.read_u32(0x80000000), c.hash_mem([[0x90000000, 0x4000000]]))
# On exit: `quit`, then kill if it hangs. The dir is removed on success, kept on exception.

# Two players through a simulated network.
with two_player_netplay("typical", rollback=True, seed=1) as s:
    host, joiner = s.clients
    print(host.netplay_status().rollback)           # current_frame, rollbacks, desyncs_detected...
    cmp = s.compare_state([[0x90000000, 0x4000000]], key=FrameKey.netplay())
    print(cmp.describe())
    print(s.netsim.format_stats())
```

The client has a typed helper for every command, plus some conveniences:

- read helpers: `read_u8/u16/f32`;
- waiting: `wait_state`, `wait_until`, `wait_frames`.

All errors are `HarnessError` subclasses:

| error | meaning |
|---|---|
| `HarnessCommandError` | the server answered `ok: false`; it carries `.cmd` and `.message` |
| `WaitTimeoutError` | `wait_frame` ran out of time |
| `HarnessTimeoutError` | no reply within the client timeout; the connection stays usable and the late reply is discarded |
| `HarnessConnectionError` | could not connect, or the connection dropped |
| `HarnessBusyError` | another client is already connected |
| `VersionMismatchError` | the server speaks a different protocol version |

`HarnessClient.connect_with_retry(port, is_alive=proc_alive)` retries while Dolphin boots, and stops at once if the process dies.

`DolphinInstance` details:

- **Template copy.** The 2 GB `Wii/sd.raw` is copied with the OS fast path: `CopyFileW` on Windows (about 0.5 s when the file is in cache), a reflink on Linux, `clonefile` on macOS. The read-only `Load/` texture packs (about 800 MB) are linked (a symlink, or a junction on Windows) instead of copied. The template's old logs are not copied.
- **Config.** `InstanceConfig` writes into `Dolphin.ini` and `GCPadNew.ini`:
  - `cpu_thread`, `video_backend`, `emulation_speed`;
  - `standard_controllers` (`SIDevice = 6`; other ports become None);
  - direct netplay (no traversal or UPnP), analytics off;
  - free-form `dolphin_ini` / `gcpad_ini` / `config_args` (`-C`).
- **Process.** The process gets its own process group. On Windows it is tied to a kill-on-close Job object, and on Linux to `PDEATHSIG`, so killing the Python driver also kills its Dolphins. An `atexit` hook kills anything left over.
- **Shutdown order.** `quit`, then SIGTERM (POSIX only), then kill.
- **Output.** stdout and stderr go to `harness-stdout.txt` and `harness-stderr.txt` in the instance dir. `d.log` tails `Logs/dolphin.log`.

## Writing a test

Tests live in `harness/tests/`. Fixtures are in `conftest.py`:

- `make_instance(name, **kw)`: instances backed by **`fake_dolphin`**. That is a Python process that accepts Dolphin's command line and runs the mock server, so tests need no build. Use `env={"PPH_FAKE": "ignore_quit|crash|no_harness|listen_delay=S"}` to simulate misbehaviour.
- `dolphin(name=None, **kw)`: real Dolphin instances in `run/instances/<test-name>-<n>`. Mark these tests `@pytest.mark.dolphin`. They are skipped automatically if the binary is missing, or if it doesn't answer `ping` on `--harness-port` (probed once per session).
- When a test fails, its instance dirs are **kept** for debugging. Use `--keep` to keep them always.

```python
import time
import pytest
from ppharness.session import FrameKey, compare_state, two_player_netplay

@pytest.mark.dolphin
def test_rollback_survives_typical_network(request, dolphin_exe):
    with two_player_netplay("typical", exe=dolphin_exe, name=request.node.name) as s:
        host, joiner = s.clients
        joiner.pad_script(0, [{"buttons": ["A"], "hold": 2}, {"hold": 30}])
        while host.netplay_status().rollback.get("current_frame", 0) < 600:
            time.sleep(0.5)
        for c in (host, joiner):
            assert c.netplay_status().rollback["desyncs_detected"] == 0
        cmp = compare_state(s.instances, [[0x80000000, 0x100]], key=FrameKey.netplay())
        assert cmp.match, cmp.describe()
```

Game-level flows live in `ppharness/brawl.py` (memory map and recipes, see `docs/brawl-memory-map.md`) and `ppharness/flows.py` (seats, CSS/SSS/match flows, a chase/random fighter, `diagnose`). `tests/test_e2e_match.py` drives Fox vs Falco offline (Battlefield, FD) and over fixed-delay and rollback netplay; artifacts go to `run/artifacts/<test>/`.

Tools (`harness/tools/`):

- `determinism.py record|compare|matrix`: offline determinism with identical inputs (see `docs/determinism-findings.md`).
- `qa_reachability.py`: can a netplay player reach the Code Menu, Debug Mode, Giga Bowser/Wario-Man or non-Versus modes?

Two *offline* boots are not bit-identical, even with identical inputs from the same poll index: whole-MEM1 and whole-MEM2 hashes differ (checked on the real build). So determinism tests should hash specific game-state ranges, not all of memory.

Running the suite:

```sh
cd harness
..\.venv\Scripts\python -m pytest              # everything (real-Dolphin tests run if supported)
..\.venv\Scripts\python -m pytest -m "not dolphin and not slow"   # quick, no build needed
..\.venv\Scripts\python -m pytest -m dolphin --keep               # only the end-to-end tests
```

## Comparing state at "the same frame"

Two Dolphins never run in lockstep, so `compare_state` has to decide what "the same frame" means and stop every instance there. The full write-up is in the `session.py` docstring; the short version follows.

The **key** (`FrameKey`) decides what counts as the same frame:

| key | where it comes from | use it for |
|---|---|---|
| `input_polls()` | game frames since boot (Brawl's own pad reads) | offline determinism tests with identical boots |
| `frame()` | VI fields since boot | weakest: also ticks during loads |
| `memory(addr)` | a u32 frame counter in game memory | when you know where the game keeps one |
| `netplay()` | `netplay_status.rollback.current_frame` (the GekkoNet frame) | rollback netplay |

The **method** decides how instances are stopped:

- **`"pause"`** (exact, the default). All instances in parallel: `wait_frame` until a few frames before the target, then `pause`, then `frame_advance` one field at a time until the key equals the target. Then `hash_mem` on all, and resume. If an instance overshoots, all are resumed and it retries further ahead.
- **`"sample"`** (never pauses). Repeats `key, hash_mem, key` on each instance and compares hashes at a key value seen on all of them. This only works for memory that doesn't change within a frame. It flags samples that turned out unstable.

Limits:

- Under rollback, the peer that is ahead may hold **speculative** state for the compared frame, so a mismatch is not proof of a desync. Compare after a stretch of unchanged input, or use the confirmed-frame hash history proposed in the protocol doc.
- Whole MEM1 differs between netplay peers even without a desync (per-instance data). Hash the ranges that hold game state.
- `pause` during netplay works in practice, but long pauses may trip netplay timeouts.

## netsim: simulated network conditions

`ppharness.netsim.NetSim` is a userspace UDP proxy. It listens on one port and forwards to the real host port. Each client address gets its own upstream socket, so the host sees one peer per client. Both directions are impaired independently: `up` is client to host, `down` is host to client.

```sh
python -m ppharness.netsim --listen 127.0.0.1:2627 --forward 127.0.0.1:2626 --preset typical
python -m ppharness.netsim --listen 0.0.0.0:2627 --forward 10.0.0.5:2626 --preset good \
    --down-loss 0.02 --spike 30:0.3:loss=0.5 --seed 7 --stats-interval 5     # e.g. on a VPS
python -m ppharness.netsim --list-presets
```

Every link knob exists for both directions (`--latency`), for `up` only (`--up-latency`) and for `down` only (`--down-latency`):

| knob | flag |
|---|---|
| base latency (ms) | `--latency` |
| jitter (ms) | `--jitter` |
| jitter distribution | `--dist normal\|uniform\|pareto` |
| jitter correlation time (ms) | `--jitter-corr` |
| random loss | `--loss` |
| Gilbert-Elliott burst loss | `--burst RATE[:MEANLEN]` |
| duplication | `--dup` |
| explicit reordering | `--reorder`, `--reorder-delay` |
| bandwidth cap | `--rate-kbps`, with a drop-tail queue set by `--queue-ms` |
| ordering | `--order fifo\|none` |

Other options: `--spike AT:DUR[:loss=..,latency=..,jitter=..,dir=up|down]` schedules a temporary impairment, and `--seed` makes runs reproducible. From Python, `set_profile`, `set_link`, `add_spike` and `reset_epoch` change conditions while the proxy runs.

**Presets** are round-trip ("ping") figures. Each direction gets half the latency and `jitter/sqrt(2)`. Loss is per direction.

| preset | RTT | loss |
|---|---|---|
| `lan` | 1 ms | 0 |
| `good` | 15 ± 2 ms | 0 |
| `typical` | 40 ± 8 ms | 0.5 % |
| `bad_wifi` | 60 ± 25 ms | 2 % in bursts (mean 3 packets) |
| `cross_country` | 80 ± 5 ms | 0 |
| `awful` | 150 ± 50 ms | 5 % |
| `none` | no impairment | 0 |

How it works:

- **Model.** Per link (client × direction), each packet goes through:
  1. loss: independent, plus Gilbert-Elliott;
  2. the bandwidth queue;
  3. latency + jitter;
  4. the ordering rule;
  5. duplication.
- **Reproducible fates.** Every packet consumes exactly six uniforms from its link's own seeded RNG (`random.Random(f"{seed}:{client}:{direction}")`). So with the same seed, the n-th packet of a link gets the same fate whatever the timing. The exceptions are the bandwidth queue and time-based spikes, which depend on the wall clock.
- **Ordering.** Real paths are FIFO. `order=fifo` (the default) never lets a packet overtake the one in front; it waits behind it instead. Sparse traffic, such as pings or Dolphin's netplay (about 14 packets/s observed), sees exactly the configured distribution. A dense stream whose jitter is large compared with the packet spacing gets "compressed" behind slow packets, which raises its mean (numbers below). `order=none` gives each packet its own sampled delay, like netem, so jitter reorders packets.
- **Timing.** A heap of deadlines is served by one thread on `time.perf_counter` (QPC on Windows; `time.monotonic` is 15.6 ms coarse on Windows in Python 3.12). On Windows the timer resolution is raised to 1 ms while netsim runs, and the netsim threads get above-normal priority. Waits use `Condition.wait` until 2 ms before the deadline, then sub-millisecond `time.sleep`, then a spin that releases the GIL.

### Measured accuracy

Measured on Windows 11 with `python -m ppharness.netsim_bench`: 60 packets/s for 15 s, sender, proxy and echo server in one process, so the one-way delays are exact. Scheduling error (actual minus target send time) had p99 ≤ 0.5 ms in every row. The fixed overhead is about 0.15 ms per direction (the `lan` row: 0.5 ms configured, 0.64 ms measured).

| preset | order | RTT p10 / p50 / p90 / p99 (ms) | RTT sd | one-way p50, sd (configured) |
|---|---|---|---|---|
| lan | fifo | 1.2 / 1.2 / 1.3 / 1.6 | 0.09 | 0.64, 0.06 (0.5, 0) |
| good | fifo | 13.0 / 15.4 / 17.8 / 19.7 | 1.91 | 7.79, 1.37 (7.5, 1.41) |
| typical | fifo | 31.5 / 40.6 / 50.3 / 57.9 | 7.25 | 20.52, 5.35 (20, 5.66) |
| typical | none | 30.3 / 40.3 / 50.2 / 57.9 | 7.69 | 20.54, 5.48 |
| bad_wifi | fifo | 48.7 / 69.8 / 95.0 / 116.0 | 18.1 | 34.3, 14.1 (30, 17.7) |
| bad_wifi | none | 30.9 / 61.9 / 89.8 / 115.2 | 23.3 | 31.4, 16.4 |
| cross_country | fifo | 74.3 / 80.3 / 86.5 / 91.0 | 4.72 | 40.39, 3.42 (40, 3.54) |
| awful | fifo | 147.7 / 186.0 / 231.2 / 276.3 | 33.1 | 91.9, 25.7 (75, 35.4) |
| awful | none | 89.9 / 151.9 / 208.8 / 267.5 | 47.1 | 77.6, 33.7 |

Loss over UDP, 10,000 packets per direction:

| preset | configured | up | down |
|---|---|---|---|
| typical | 0.5 % | 0.57 % | 0.42 % |
| bad_wifi | 2 % | 1.89 % | 2.00 % |
| awful | 5 % | 5.23 % | 5.10 % |

The model tests check loss, burst length, duplication and reordering over 100k-300k packets, within statistical bounds.

For the two noisiest presets, FIFO compression of a 60 packets/s stream adds about 8 ms (`bad_wifi`) and about 34 ms (`awful`) to the median RTT. Use `--order none` when you need the exact distribution per packet.

## Layout

```
ppharness/
  protocol.py      constants, address/button validation
  client.py        HarnessClient + typed results + errors
  instance.py      DolphinInstance, InstanceConfig, template copy, clean, probe
  inifile.py       order-preserving Dolphin INI editor
  logtail.py       dolphin.log tailing / wait_for(regex)
  netsim.py        UDP impairment proxy + CLI
  netsim_bench.py  accuracy measurement
  session.py       two_player_netplay, compare_state, pause_at, FrameKey
  mock_server.py   protocol implementation with fake memory/frames/netplay
  fake_dolphin.py  DolphinNoGUI stand-in running the mock server
  _platform.py     everything OS-specific
  cli.py           python -m ppharness
tests/             pytest suite (test_dolphin.py needs the real build)
```
