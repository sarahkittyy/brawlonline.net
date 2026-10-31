export const GameSettingsMessages = {
  meleeIsoFile: () => "Brawl Disc Image",
  meleeIsoFileDescription: () =>
    "Your NTSC-U Super Smash Bros. Brawl disc image (revision 1 or 2), as an ISO, RVZ or WBFS.",
  closeDolphinToChange: () => "Close Dolphin to change this setting",
  noFileSet: () => "No file set",
  verifying: () => "Verifying...",
  playButtonAction: () => "Play Button Action",
  launchMelee: (gameName: string) => "Launch {0}",
  launchDolphin: () => "Launch Dolphin",
  inputDelay: () => "Input Delay",
  inputDelayDescription: () =>
    "More input delay = less rollbacks. Increase this if your games are stuttering / teleporting around. " +
    "Automatic picks it from your ping at the start of each game. Takes effect right away, even in the middle of a game.",
  inputDelayAutomatic: () => "Automatic",
  inputDelayFixed: () => "Fixed",
  inputDelayAggressive: () => "Aggressive",
  inputDelayNormal: () => "Normal",
  inputDelaySmooth: () => "Smooth",
  inputDelayFrames: (frames: number) => (frames === 1 ? "1 frame" : `${frames} frames`),
  unrankedRatingRange: () => "Unranked Rating Range",
  unrankedRatingRangeDescription: () =>
    "Only match with players whose Ranked rating is this close to yours in Unranked. Searches can take much longer. Works once you have placed in Ranked, and then skips players who haven't placed yet. If your opponent also set a range, the smaller one counts.",
  unrankedRatingRangeOff: () => "Off",
  unrankedRatingRangePoints: (points: number) => "±{0}",
  unrankedRatingRangeCustom: () => "Custom",
  unrankedRatingRangePreview: (rating: string, low: string, high: string, lowRank: string, highRank: string) =>
    "You're at {0}. You'll be matched with placed players rated {1} to {2} ({3} to {4}).",
  unrankedRatingRangeNotPlaced: (sets: number) =>
    sets === 1
      ? "This starts working once you place in Ranked (1 more set). Until then you search as if it were off."
      : `This starts working once you place in Ranked (${sets} more sets). Until then you search as if it were off.`,
  unrankedRatingRangeNoProfile: () => "Log in to see which ratings this matches you with.",
  enableMusic: () => "Music",
  enableMusicDescription: () => "Play the game's music in menus and matches.",
  disableChat: () => "Disable Chat",
  disableChatDescription: () => "Hide the chat window everywhere in the game.",
  enableStreamGames: () => "Show my games on the Spectate tab",
  enableStreamGamesDescription: () =>
    "Streams inputs to the server for others to spectate you. Very minimal network load. Your opponent's setting may still show the match. Ranked games are always sent to the server to check results, and shown only if a player allows it.",
  netplayNetwork: () => "Network for online games",
  netplayNetworkDescription: () => "The network interface you connect to peers with.",
  netplayNetworkAuto: () => "Automatic",
  netplayNetworkMissing: (address: string) => "{0} (not connected)",
  closeDolphinAfterSpectating: () => "Close Dolphin after a spectated game ends",
  closeDolphinAfterSpectatingDescription: () => "Auto play on the Spectate tab keeps it open.",
  chatKey: () => "Chat Key",
  chatKeyPressAKey: () => "Press a key...",
  chatKeyEscToCancel: () => "Esc to cancel",
  chatKeyRefused: () => "Use a letter, number, F key or punctuation key, with no Ctrl, Alt or Shift.",
  chatKeyReset: () => "Reset to T",
  valid: () => "Valid",
  unknown: () => "Unknown",
  invalid: () => "Invalid",
  unvalidated: () => "Unvalidated",
};
