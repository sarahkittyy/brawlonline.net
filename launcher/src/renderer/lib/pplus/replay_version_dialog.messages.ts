export const ReplayVersionDialogMessages = {
  titleReplay: (version: string) => "This replay is from Project+ {0}",
  titleClip: (version: string) => "This clip is from Project+ {0}",
  explain: (version: string, current: string) =>
    "You're on Project+ {1}. You can download a small patch to have access to replays from {0}.",
  explainRender: (version: string, current: string) =>
    "You're on Project+ {1}. You can download a small patch to render clips from replays from {0}.",
  download: (size: string) => "Download: {0}",
  diskSpace: (version: string) => "Disk: 2 GB for a copy of {0}'s SD card. You can remove it in Settings > Replays.",
  downloadAndPlay: () => "Download and play",
  downloadAndRender: () => "Download and render",
  playAnyway: (current: string) => "Play on {0} anyway",
  haveIt: (version: string) => "I have Project+ {0} on this PC",
  chooseFolderTitle: (version: string) => "Choose your Project+ {0} folder (or its sd.raw)",
  cancel: () => "Cancel",
  workingTitle: (version: string) => "Setting up Project+ {0}",
  stepFiles: () => "Downloading...",
  stepImport: () => "Grabbing your files",
  stepCard: () => "Making the replay SD card",
  errorTitle: (version: string) => "Couldn't get Project+ {0}",
  errorOffline: () => "Brawl Online couldn't download the files. Check your internet connection and try again.",
  errorDamaged: () => "The download didn't come through correctly. Try again.",
  errorCardUnknown: (message: string) => "{0}",
  errorOther: (message: string) => "Something went wrong: {0}",
  retry: () => "Try again",
  unplayableTitle: (version: string) => "This replay is from Project+ {0}",
  unplayable: (version: string) =>
    "This version of Brawl Online can't play Project+ {0} replays. You can still try to play them on this version.",
  unplayableRender: (version: string) => "This version of Brawl Online can't render Project+ {0} replays.",
  ok: () => "OK",
  skipped: (played: string) =>
    "Some of the ones you selected were from a different Project+ version. Playing only the Project+ {0} replays. Select the other ones on their own to watch them.",
};
