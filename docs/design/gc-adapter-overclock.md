# GameCube adapter overclock (research, 2026-11-24)

Status: research only, nothing built. Sarah's board: "investigate online for the commonly
recommended / posted about methods to overclock your GCC adapter ... see what it would take to
integrate it directly into our launcher / installer. it'd have to be optional but visible,
available to fresh installs but also as a one click setup and verification thing in the launcher".

## What players do now

The official adapter (WUP-028, 057E:0337) is a Full Speed USB device whose interrupt endpoint
asks for bInterval 8, so the PC polls it at 125 Hz. "Overclocking" means making the PC poll it every
1 ms instead. The controller's state then reaches Dolphin up to 1 ms old instead of up to 8 ms old.
The guide below puts the average saving at about 3.5 ms on the official adapter and 14 ms on a
Mayflash. At 60 fps that is also less of the jitter that pushes a press into the next frame.

- **Windows**: Arte's Google Doc "Overclocking your GCC to USB Adapter + ports to use" (the
  guide the Melee/Slippi scene links), using SweetLow's **HIDUSBF**
  (github.com/LordOfMice/hidusbf). The steps: keep the Zadig WinUSB driver (HIDUSBF is a *lower
  filter* under WinUSB, not a replacement), run its Setup.exe as admin, Install Service, show All
  devices, pick WUP-028, check Filter On Device, Selected Rate 1000, Restart. To check it, look at
  Setup's bInterval column (should be 1) or at Slippi's/Dolphin's adapter poll rate (1000 ± 100 Hz,
  with some ~500 Hz readings being normal). To undo it, reinstall WinUSB with Zadig. Most of the
  guide is about errors 577 and 193 (signature/Secure Boot, Memory Integrity), and the
  `allow_overclock.reg` (CI\Policy "UpgradedSystem") / Secure Boot off workarounds.
- **macOS**: a third-party kext with SIP loosened (Slippi Ishiiruka wiki), or GCAdapterDriver.
  Not something a launcher should do.
- **Linux**: an out-of-tree kernel module (hannesmann/gcadapter-oc-kmod, DKMS packages), or a
  patched kernel with `usbcore.interrupt_interval_override` (not in mainline). Needs root, kernel
  headers, and a signed module with Secure Boot. Not one-click either.
- **Adapters that are fast already**: Lossless, HandHeldLegend GC Pocket+ (its "Slippi" mode),
  and other boards that report the WUP-028 IDs but bInterval 1 run at 1000 Hz with no driver.
  Sarah's own adapter is a GC Pocket+ (instance `GCP+`, WinUSB, no filter). Cheap clones stay
  near 125 Hz whatever the driver does. Mayflash firmware v6+ reads about 500 Hz, and the 2-port
  Mayflash 500 Hz.

## Why Windows is doable now

The guide's pain points come from old builds and no longer apply to the driver we would use:

- Since 2024-07 HIDUSBF's `AMD64_AS` drivers are **Microsoft attestation-signed** (checked:
  "Microsoft Windows Hardware Compatibility Publisher"), so Secure Boot and the CI policy .reg
  aren't needed on Windows 10/11 x64.
- Since 2025-04 the **NoPatch** `AMD64_AS` build meets Memory Integrity (HVCI). The README says
  "overclocking of Full Speed USB devices is possible always and there is no need to patch system
  code". The adapter is Full Speed, so NoPatch is enough, and error 193 doesn't happen.
- What it does (from HIDUSBF_AS.INF and the driver's strings): a demand-start kernel service
  `hidusbf` (`System32\drivers\hidusbf.sys`, 40 KB), `hidusbf` in the device instance's
  **LowerFilters**, and a DWORD **`bInterval`** in the instance's Device Parameters key. The driver
  reads that value and rewrites the endpoint's bInterval. After that, the device is restarted.
- Pinned file: master 8bfc7608 (2026-10-03), `DRIVER/AMD64_AS/NoPatch/hidusbf.sys` sha256
  `2f82cdeb36bdaa42ea1933a9b11f3b8e1bdb28e6d3e3da7e65b4631b3375412d`. There is no ARM64 build
  (author, issue #409: "Not near"), and there is no silent install switch.

## What integrating it would take

1. **Permission (blocker)**: the repo has **no license**. Bundling hidusbf.sys needs SweetLow's
   OK (contact in README.ENG.TXT; Battle Beaver Customs pays for the signing). The fallback is to
   download the pinned zip from his GitHub when the player clicks, and check its SHA-256.
2. **A small elevated helper** (`gc-adapter-oc.exe`, C++, about 300 lines, beside
   gc-driver-install.exe in `launcher/assets`). Admins can't write Enum keys directly, so it has to
   go through SetupDi:
   - `status` (no admin): every 057E:0337 instance Windows has seen, with its service (WinUSB?),
     LowerFilters, bInterval, the `hidusbf` service state, and the endpoint's native bInterval.
   - `on [rate]`: copy the driver and create the service (what the INF does), set LowerFilters with
     `SetupDiSetDeviceRegistryProperty(SPDRP_LOWERFILTERS)`, set `bInterval` with
     `SetupDiOpenDevRegKey(DIREG_DEV)`, and restart the device (DIF_PROPERTYCHANGE). This works for
     unplugged instances too, so they come up at 1000 Hz the next time they're plugged in. **If the
     device doesn't come back healthy within ~10 s (the filter didn't load: Code 39/31, so the
     adapter would be dead), it undoes everything at once.**
   - `off`: remove the filter and the value, restart the device. The service can stay, as the
     guide's rollback leaves it.
   - `measure` (no admin): open the adapter with WinUSB and time about 2 s of reads, giving N Hz
     and the share of ~500 Hz reads. This only works while no Dolphin holds the adapter. Otherwise
     use Dolphin's own figure: `GCAdapter::GetCurrentPollRate()` is already measured, and
     machine sessions log "adapter at N Hz" in the `input:` line.
3. **Installer**: under "Also install GameCube adapter drivers", add an "overclock" checkbox that
   runs after the driver (the filter needs WinUSB under it). It would only apply to adapters this
   PC has seen. 64df55ba9 already keeps an existing HIDUSBF filter when the driver is present.
   Running gc-driver-install.exe (libwdi) later would drop the filter again, and the launcher's
   status would show that.
4. **Launcher (Windows only)**: one settings row that shows the adapter's state and has one button.
   The states: no adapter seen / no driver (install it: the launcher doesn't run
   gc-driver-install.exe today) / already 1000 Hz natively / overclocked, measured N Hz, undo /
   125 Hz, overclock / overclocked but still low (Mayflash firmware, hub, clone). The button gives
   one UAC prompt. To make it "visible", the row could be pointed to when the launcher sees a
   125 Hz adapter (Dolphin's reading or the helper's). On macOS and Linux the row shows the rate
   and links the guides. All wording would be Sarah's.
5. **Testing**: on Sarah's GC Pocket+, *down*clock to 250 Hz (README: if the rate drops, the
   driver works), then undo. Then a real official or Mayflash adapter for the 125 to 1000 Hz path.
   Also check with Memory Integrity on, with an unplugged adapter, and the auto-undo with the
   driver file blocked.

Rough size: helper 1-2 days, launcher row + IPC 1 day, installer 0.5 day, plus testing on a real
WUP-028. Open risks: no license yet. Some anti-cheats or antivirus programs may dislike a USB
filter driver (not checked). A firmware that NAKs at 1 ms reads about 500 Hz, which is
still better than 125.

Sources: the Google Doc (docs.google.com/document/d/1cQ3pbKZm_yUtcLK9ZIXyPzVbTJkvnfxKIyvuFMwzWe0),
github.com/LordOfMice/hidusbf (README, issues #54, #356, #409), Slippi Ishiiruka wiki
"Overclocking Controllers on macOS ...", github.com/hannesmann/gcadapter-oc-kmod,
github.com/GloriousEggroll/Linux-Pollrate-Patch.
