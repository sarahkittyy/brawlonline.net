export const ReplayVersionDialogMessages = {
  titleReplay: (version: string) => "This replay is from Project+ {0}",
  titleClip: (version: string) => "This clip is from Project+ {0}",
  explain: (version: string, current: string) =>
    "You're on Project+ {1}. A replay only plays the way the game happened on the Project+ version it was recorded on, so Brawl Online can keep Project+ {0} just for replays. Online play stays on {1}.",
  explainRender: (version: string, current: string) =>
    "You're on Project+ {1}. A clip only renders the way the game happened on the Project+ version it was recorded on, so Brawl Online can keep Project+ {0} just for replays and clips. Online play stays on {1}.",
  download: (size: string) => "Download: {0}, once",
  diskSpace: () => "Disk: about 2 GB for a replay copy of your SD card (remove it any time in Settings > Replays)",
  dontAsk: () => "Don't ask again: get older versions for replays when they're needed",
  downloadAndPlay: () => "Download and play",
  downloadAndRender: () => "Download and render",
  playAnyway: (current: string) => "Play on {0} anyway",
  haveIt: (version: string) => "I have Project+ {0} on this PC",
  chooseFolderTitle: (version: string) => "Choose your Project+ {0} folder (or its sd.raw)",
  cancel: () => "Cancel",
  workingTitle: (version: string) => "Getting Project+ {0} for replays",
  stepFiles: (version: string) => "Downloading Project+ {0}'s files",
  stepImport: (version: string) => "Taking Project+ {0}'s files from your folder",
  stepCard: () => "Making the replay SD card (one time)",
  workingNote: () => "This happens once. Next time this version's replays start right away.",
  errorTitle: (version: string) => "Couldn't get Project+ {0}",
  errorOffline: () => "Brawl Online couldn't download the files. Check your internet connection and try again.",
  errorDamaged: () => "The download didn't come through correctly. Try again.",
  errorCardUnknown: (message: string) => "{0}",
  errorOther: (message: string) => "Something went wrong: {0}",
  retry: () => "Try again",
  unplayableTitle: (version: string) => "This replay is from Project+ {0}",
  unplayable: (version: string, current: string) =>
    "This version of Brawl Online can't play Project+ {0} replays yet. Update Brawl Online, or play it on Project+ {1} anyway: it may play differently or stop early.",
  unplayableRender: (version: string) =>
    "This version of Brawl Online can't render Project+ {0} replays yet. Update Brawl Online to render this clip.",
  ok: () => "OK",
  skippedOne: (version: string, played: string) =>
    "Playing the Project+ {1} replays. The one from Project+ {0} plays on its own: select it by itself.",
  skippedMany: (count: number, version: string, played: string) =>
    "Playing the Project+ {2} replays. The {0} from Project+ {1} play on their own: select them by themselves.",
};
