import type { EmissaryStatus } from "@dolphin/emissary/types";

export const AddonsSettingsMessages = {
  emissary: () => "EmissaryRecomp",
  beta: () => "BETA",
  by: () => "by",
  emissaryDescription: () =>
    "Brawl as a native PC program instead of Dolphin, with Brawl Online's rollback. It plays Project+ online with Dolphin players. Other Games and replays still use Dolphin.",
  installed: () => "Installed",
  latest: () => "Latest",
  status: () => "Status",
  program: () => "Program",
  gameFiles: () => "Game files",
  fromDisc: () => "from your disc image",
  notInstalled: () => "Not installed",
  unknown: () => "Unknown",
  download: (download: string, disk: string) => `Download (${download} download, ${disk} on disk)`,
  update: (size: string) => `Update (${size})`,
  prepare: (disk: string) => `Prepare game files (${disk})`,
  cancel: () => "Cancel",
  remove: () => "Remove",
  removeConfirm: () => "Remove EmissaryRecomp? Its program, game files, settings and saves are deleted. Replays stay.",
  openFolder: () => "Open folder",
  autoUpdate: () => "Keep up to date automatically",
  autoUpdateDescription: () => "Newer releases download by themselves and play the next time the game starts.",
  playWith: () => "Play with it in Settings > Game.",
  noOnline: () => "This release can't play online yet: Play uses Dolphin until an update can.",
  // Play with EmissaryRecomp chosen, but Dolphin played: why (its state, or its update's error).
  playUsesDolphin: (why: string) => `EmissaryRecomp: ${why} Play uses Dolphin.`,
  chooseDisc: () => "Choose disc image",
  staged: (version: string) => `${version} is downloaded and plays the next time the game starts.`,
  jobDownload: () => "Downloading",
  jobUnpack: () => "Unpacking",
  jobGameFiles: () => "Extracting the game files from your disc image",
  statusText: (status: EmissaryStatus): string => {
    switch (status) {
      case "unavailable":
        return "Not available on this computer yet (Windows and Linux only).";
      case "needsDisc":
        return "Needs your Brawl disc image: choose it in Settings > Game first.";
      case "notInstalled":
        return "Not installed.";
      case "downloading":
        return "Downloading.";
      case "preparingFiles":
        return "Extracting the game files.";
      case "needsFiles":
        return "Its game files are missing or from another disc image.";
      case "ready":
        return "Ready.";
      case "updateAvailable":
        return "An update is available.";
      case "running":
        return "Playing.";
    }
  },
};
