export const DolphinMessages = {
  dolphinIsUpdating: () => "Dolphin is updating. Try again later.",
  updateLauncherFirst: (version: string) =>
    "Version {0} is out. Update the launcher before starting Dolphin: the bar at the bottom of the window downloads it, then click Install update there.",
  failedToInstallDolphin: (dolphinTypeName: string) =>
    "Failed to install {0}. Try closing all Dolphin instances and restarting the launcher.",
  netplayDolphin: () => "Netplay Dolphin",
  playbackDolphin: () => "Replay Dolphin",
};
