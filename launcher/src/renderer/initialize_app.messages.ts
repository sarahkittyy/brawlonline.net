export const InitializeAppMessages = {
  failedToCommunicateWithServers: (productName: string) => "Failed to communicate with {0} servers.",
  youAreOffline: () => "You are offline.",
  serversMayBeDown: (productName: string) =>
    `{0} may be experiencing some downtime. Playing online may or may not work.`,
  failedToInstallDolphin: (dolphinTypeName: string) =>
    "Failed to find {0}. Check the Dolphin executable in Settings and restart the launcher.",
  netplayDolphin: () => "Netplay Dolphin",
  playbackDolphin: () => "Playback Dolphin",
  updatedToVersion: (productName: string, version: string) => `{0} has been updated to version {1}`,
  updateFailed: (version: string) => `Auto-update to version {0} failed. Try manually downloading the latest version.`,
};
