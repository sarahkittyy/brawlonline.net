import type { EmissaryStatus } from "@dolphin/emissary/types";

export const AddonsSettingsMessages = {
  emissary: () => "EmissaryRecomp",
  beta: () => "BETA",
  by: () => "by",
  // Sarah's words (board, 2026-11-24).
  emissaryDescription: () =>
    'Play P+ as a native PC program instead of Dolphin. Uses Brawl Online\'s netcode, you can cross-play with Dolphin players. "Other Games" rooms still use Dolphin.',
  madeBy: () => "Made by",
  // The card's Dolphin | Emissary bar: Settings > Game's "Play Project+ with", right = EmissaryRecomp.
  playWithEmissaryOn: () => "Emissary",
  playWithEmissaryOff: () => "Dolphin",
  installed: () => "Installed",
  latest: () => "Latest",
  notInstalled: () => "Not installed",
  unknown: () => "Unknown",
  download: (download: string, disk: string) => `Download (${download} download, ${disk} on disk)`,
  update: (size: string) => `Update (${size})`,
  prepare: (disk: string) => `Prepare game files (${disk})`,
  cancel: () => "Cancel",
  remove: () => "Remove",
  removeConfirm: () => "Remove EmissaryRecomp? Its program, game files, settings and saves are deleted. Replays stay.",
  openFolder: () => "Open folder",
  autoUpdate: () => "Update automatically",
  noOnline: () => "This release can't play online yet: Play uses Dolphin until an update can.",
  // Play with EmissaryRecomp chosen, but Dolphin played: why (its state, or its update's error).
  usingDolphin: (why: string) => `EmissaryRecomp: ${why} Using Dolphin.`,
  chooseDisc: () => "Choose disc image",
  staged: (version: string) => `${version} is downloaded and plays the next time the game starts.`,
  jobDownload: () => "Downloading",
  jobUnpack: () => "Unpacking",
  jobGameFiles: () => "Extracting the game files from your disc image",
  // The sidebar's set-up status while it downloads (as "Downloading Project+").
  downloadingEmissary: () => "Downloading EmissaryRecomp",
  extractingGameFiles: () => "Extracting game files",
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
