const { isLinux, isWindows } = window.electron.bootstrap;

import { HandleDolphinExitCodeMessages as Messages } from "./handle_dolphin_exit_code.messages";

// EmissaryRecomp closed itself because the graphics driver took its device away and it could not
// make a new one (it does up to 3 times a run, the 2nd on Vulkan moving to Direct3D 12): the
// recomp's aurora_events.h kExitGraphicsDeviceLost.
const EMISSARY_GRAPHICS_DEVICE_LOST = 0x4c;

export const handleDolphinExitCode = (exitCode: number | null): string | undefined => {
  if (exitCode == null || exitCode === 0) {
    return undefined;
  }

  let err: string | undefined;

  if (isWindows) {
    err = handleWindowsExitCode(exitCode);
  }

  if (isLinux) {
    err = handleLinuxExitCode(exitCode);
  }

  return err;
};

const handleWindowsExitCode = (exitCode: number): string | undefined => {
  switch (exitCode) {
    case 0x3: {
      // returned when selecting update in game
      return undefined;
    }
    case 0xc0000135:
    case 0xc000007b: {
      return Messages.requiredDllsMissing();
    }
    case 0xc0000409: {
      return Messages.dolphinCrashed();
    }
    case 0xc0000005: {
      return Messages.tryDifferentVideoBackend();
    }
    case 0xcfffffff: {
      // Windows closed it while "Not responding" (the player chose Close the program). Dolphin's
      // hang watchdog left a dump of what it was stuck on in its Logs, which a bug report uploads.
      return Messages.dolphinStoppedResponding();
    }
    case EMISSARY_GRAPHICS_DEVICE_LOST: {
      return Messages.emissaryGraphicsDeviceLost();
    }
    default: {
      const errorCode = `0x${exitCode.toString(16)}`;
      return Messages.dolphinExitedWithErrorCode(errorCode);
    }
  }
};

const handleLinuxExitCode = (exitCode: number): string => {
  switch (exitCode) {
    case 0x7f: {
      return Messages.requiredLibrariesMissing();
    }
    case EMISSARY_GRAPHICS_DEVICE_LOST: {
      return Messages.emissaryGraphicsDeviceLost();
    }
    default: {
      const errorCode = `0x${exitCode.toString(16)}`;
      return Messages.dolphinExitedWithErrorCode(errorCode);
    }
  }
};
