// A crash of the launcher's GPU process can take more down with it on Linux: on a dual-AMD
// Framework 16 under Hyprland (2026-10-16) the GPU process segfaulted in the driver
// ("amdgpu: Move buffer fallback to memcpy unavailable") and the compositor aborted right after,
// ending the desktop session. --disable-gpu avoided it. So on Linux the first such crash turns
// Settings > Advanced > Hardware Acceleration off for the next start; the player can turn it on again.

import type { App, Details } from "electron";

/** Whether `details` (app's child-process-gone) is a GPU process crash that turns hardware acceleration off. */
export function turnsOffHardwareAcceleration(
  details: Pick<Details, "type" | "reason">,
  platform: NodeJS.Platform,
): boolean {
  // Only a crash: a GPU process that is killed or exits (the desktop session ending, the launcher
  // quitting) says nothing about the driver.
  return platform === "linux" && details.type === "GPU" && details.reason === "crashed";
}

export function watchGpuCrashes(
  app: Pick<App, "on">,
  turnOff: () => void,
  log: { warn: (...args: unknown[]) => void },
  platform: NodeJS.Platform = process.platform,
): void {
  app.on("child-process-gone", (_event, details) => {
    if (!turnsOffHardwareAcceleration(details, platform)) {
      return;
    }
    // Stored first: the desktop may end the launcher a moment later.
    turnOff();
    log.warn(
      `The GPU process crashed (exit code ${details.exitCode}); hardware acceleration is off from the next start (Settings > Advanced)`,
    );
  });
}
