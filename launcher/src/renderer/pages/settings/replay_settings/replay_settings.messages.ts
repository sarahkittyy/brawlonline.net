export const ReplaySettingsMessages = {
  rootSlpFolder: () => "Root Replay Folder",
  rootSlpFolderDescription: () => "The folder where your replays should be saved.",

  replayGraphics: () => "Replay & Spectating Graphics",
  replayGraphicsDescription: () => "Replays and spectated games play in their own Dolphin.",
  replayGraphicsButton: () => "Replay Dolphin settings",

  closeDolphinToChangeSetting: () => "Close Dolphin to change this setting",
  noFolderSet: () => "No folder set",

  additionalSlpFolders: () => "Additional Replay Folders",
  additionalSlpFoldersDescription: () =>
    "Choose any additional replay folders that should show up in the replay browser.",

  enableNetplayReplays: () => "Enable Netplay Replays",
  enableNetplayReplaysDescription: () => "Save replays for netplay games to the Root Replay Folder.",
  saveReplaysToMonthlySubfolders: (currentDate: string) => "Organize into monthly subfolders (e.g. {0}).",

  olderVersions: () => "Older Project+ Versions",
  olderVersionsDescription: (current: string) =>
    "A replay plays the way the game happened only on the Project+ version it was recorded on. These are kept just for replays: online play always uses Project+ {0}.",
  noOlderVersions: () => "None yet.",
  versionName: (version: string) => "Project+ {0}",
  versionReady: (size: string) => "On this PC: its replays play on it ({0} on disk)",
  versionNotHere: (size: string) => "Not on this PC: downloaded the first time one of its replays plays ({0})",
  versionUnplayable: () => "This version of Brawl Online can't play its replays",
  getVersion: () => "Download",
  removeVersion: () => "Remove",
  autoDownload: () => "Get older versions for replays without asking",
  autoDownloadDescription: () => "Downloads an older Project+ version's files when one of its replays plays.",
};
