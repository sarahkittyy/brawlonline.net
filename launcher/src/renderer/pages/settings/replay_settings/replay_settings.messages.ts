export const ReplaySettingsMessages = {
  rootSlpFolder: () => "Root Replay Folder",
  rootSlpFolderDescription: () => "The folder where your replays should be saved.",

  replayGraphics: () => "Replay & Spectating Graphics",
  replayGraphicsDescription: () =>
    "Replays and spectated games play in their own Dolphin, with its own resolution and other graphics settings.",
  replayGraphicsButton: () => "Replay Dolphin settings",

  closeDolphinToChangeSetting: () => "Close Dolphin to change this setting",
  noFolderSet: () => "No folder set",

  additionalSlpFolders: () => "Additional Replay Folders",
  additionalSlpFoldersDescription: () =>
    "Choose any additional replay folders that should show up in the replay browser.",

  enableNetplayReplays: () => "Enable Netplay Replays",
  enableNetplayReplaysDescription: () => "Save replays for netplay games to the Root Replay Folder.",
  saveReplaysToMonthlySubfolders: (currentDate: string) => "Organize into monthly subfolders (e.g. {0}).",
};
