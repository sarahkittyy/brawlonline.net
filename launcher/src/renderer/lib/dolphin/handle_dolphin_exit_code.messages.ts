export const HandleDolphinExitCodeMessages = {
  requiredDllsMissing: () =>
    "Required DLLs for launching Dolphin are missing. Install the Microsoft Visual C++ Redistributable (x64): Settings > Help has the link.",
  dolphinCrashed: () =>
    'Dolphin has crashed. Please go to the Help section of the settings page, click "Download logs", and send us the zip with some context regarding the crash.',
  tryDifferentVideoBackend: () =>
    "Try a different video backend in Dolphin. If the issue persists, install the latest Windows update available and then restart your computer.",
  dolphinStoppedResponding: () =>
    "Dolphin stopped responding and was closed. Please send a bug report with the bug button at the top of the launcher, saying what you were doing when it froze: it uploads what Dolphin was stuck on.",
  dolphinExitedWithErrorCode: (exitCode: string) =>
    "Dolphin exited with error code: {0}. " +
    "If you're in need of assistance, screenshot this and send it to us with some context regarding the crash.",
  dllMissingVcRedist: (name: string) =>
    "Dolphin couldn't start: {0} is missing. Install the Microsoft Visual C++ Redistributable (x64): Settings > Help has the link.",
  dllMissingMediaFeaturePack: (name: string) =>
    "Dolphin couldn't start: {0} is missing. It comes with Windows' Media Feature Pack, which \"N\" editions don't have: install it in Settings > Apps > Optional features.",
  dllMissingFromDolphin: (name: string) =>
    "Dolphin couldn't start: {0} is missing from Dolphin's folder. Your antivirus may have removed it: allow Brawl Online in it and restart the launcher.",
  dllMissingFromWindows: (name: string) =>
    "Dolphin couldn't start: Windows has no {0}. Please report this with the bug button (Settings > Help).",
  dllNotX64: (name: string) =>
    "Dolphin couldn't start: the {0} it found is not 64-bit. Repair the Microsoft Visual C++ Redistributable (x64) (Settings > Help has the link), or remove that file if something else put it there.",
  requiredLibrariesMissing: () =>
    "Required libraries for launching Dolphin may be missing. Check the Help section in the settings page for guidance.",
};
