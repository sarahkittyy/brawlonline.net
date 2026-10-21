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
    "These versions are kept to play replays from older versions, online play is always on Project+ {0}.",
  noOlderVersions: () => "None yet.",
  versionName: (version: string) => "Project+ {0}",
  versionNotDownloaded: () => "Not downloaded",
  versionUnsupported: () => "Unsupported",
  getVersion: () => "Download",
  removeVersion: () => "Remove",
};
