export const ModsMessages = {
  tabLabel: () => "Other Games",
  /** The tab with the number of public rooms, once the room list has loaded. */
  tabLabelWithCount: (count: number) => `Other Games (${count})`,
  intro: () =>
    "Play any Wii game with rollback (ex. NSMB Wii, REX, Brawl Minus, vBrawl). The whole game is shared. Players can drop in and out whenever they'd like, like plugging in a controller. Everyone in a room needs the exact same game.",
  yourBuilds: () => "Your games",
  addBuild: () => "Add game",
  addBuildTooltip: () => "A Wii or GameCube disc image.",
  addFolder: () => "Add mod folder",
  addFolderTooltip: () => "A Brawl mod's Dolphin folder, like Brawl Minus or REX.",
  help: () => "Help",
  faqTitle: () => "Help",
  faq: () => [
    {
      question: "I have the same game as my opponent, but it won't let me join!",
      answer:
        "For Brawl mods, check the ISO too. If you absolutely have the same mod, but the hashes differ, it's possible you've set Rev1 and they've set Rev2.",
    },
  ],
  close: () => "Close",
  noBuilds: () => "Add a game or a mod folder to play.",
  notHashed: () => "Not checked yet",
  hashTooltip: (_hash: string) => "The game's hash. All your friends need the exact same hash.",
  partsTooltip: () =>
    "Rooms compare these: the disc must be the same, and a different SD card is downloaded from the host when you join.",
  checking: (percent: number) => `Checking files… ${percent}%`,
  checkingDisc: () => "Checking the disc…",
  hashFailed: (name: string, error: string) => `Couldn't check ${name}: ${error}`,
  edit: () => "Edit",
  editTooltip: () => "Change this game's name or files.",
  remove: () => "Remove",
  removeTooltip: () => "Remove this game from the list (your files stay where they are).",
  host: () => "Host",
  hostTooltip: () => "Open a room for this game.",
  join: () => "Join",
  joinTooltip: () => "Join the room with this code. Its game is found among yours.",
  roomCode: () => "Room code",
  publicRoom: () => "Public",
  publicRoomTooltip: () => "Public rooms are listed for everyone. Private rooms need the code.",
  controller: () => "Controller",
  gameCube: () => "GameCube controller",
  wiiRemote: () => "Wii Remote",
  realWiiRemote: () => "Real Wii Remote",
  configure: () => "Configure",
  notSetUp: () => "Not set up yet",
  noneSetUp: (kind: string) => `No ${kind} set up in Dolphin: pick Configure.`,
  notPluggedIn: (kind: string) => `No ${kind} plugged in.`,
  playingWith: (label: string) => `Playing with: ${label}`,
  gameNeedsWiiRemote: () => "This game may need a Wii Remote.",
  parts: (disc: string, launcher: string | null, sd: string | null) =>
    [disc, launcher, sd].filter((p): p is string => !!p).join(" · "),

  // The room.
  yourRoom: () => "Your room",
  opening: () => "Starting Dolphin…",
  joiningRoom: () => "Joining the room…",
  inRoom: (code: string) => `Room ${code}`,
  copyCode: () => "Copy code",
  dismiss: () => "Dismiss",
  copied: () => "Copied",
  slot: (n: number) => `Player ${n}`,
  openSlot: () => "Open",
  closedSlot: () => "Closed",
  you: () => "you",
  hostLabel: () => "host",
  joiningPlayer: () => "joining",
  ping: (ms: number) => `${Math.round(ms)} ms`,
  kick: () => "Kick",
  kickTooltip: () => "Remove this player from the room. They can't come back for 10 minutes.",
  closeSlot: () => "Close slot",
  openSlotAction: () => "Open slot",
  leave: () => "Leave",
  leaveTooltip: () => "Leave the room. Dolphin keeps running; close it to stop.",
  left: () => "You left the room.",
  sessionError: (why: string) => `The session ended: ${why}`,
  buildName: (name: string, hash: string) => (name ? `${name} (${hash})` : hash),

  // Adding a build.
  addBuildTitle: () => "Add a game",
  addModTitle: () => "Add a mod",
  editBuildTitle: () => "Edit game",
  save: () => "Save",
  name: () => "Name",
  namePlaceholder: () => "Leave empty to use the game's name",
  disc: () => "Disc image",
  discHelp: () => "The game's disc image (for a Brawl mod: your Brawl disc).",
  launcher: () => "Mod launcher (optional)",
  launcherHelp: () => "A mod's .dol or .elf. Leave empty to play the disc.",
  sdCard: () => "SD card image (optional)",
  sdCardHelp: () => "The mod's SD card image, if it has one.",
  clear: () => "Clear",
  cancel: () => "Cancel",
  add: () => "Add",
  duplicate: (name: string) => `You already have this game: ${name}.`,
  dolphinOpen: () => "Dolphin is already open. Close it first.",

  // The game's save in rooms (the edit window).
  saveData: () => "Save data",
  saveNone: () => "No save yet. Rooms you host make one, or import your own.",
  saveSummary: (files: number, modified: string) =>
    `${files} ${files === 1 ? "file" : "files"}, last changed ${modified}`,
  saveLoading: () => "Reading the save…",
  saveNotReady: () => "You can import a save once the game has been checked.",
  saveHelp: () =>
    "Rooms you host play this save and keep your progress. When you join someone else's room, you play their save.",
  importSave: () => "Import save",
  importSaveFile: () => "From a data.bin file…",
  importSaveFolder: () => "From a Dolphin folder…",
  saveFileFilter: () => "Wii save (data.bin)",
  openSaveLocation: () => "Open save location",
  replaceSaveTitle: () => "Replace this save?",
  replaceSave: (summary: string) =>
    `This game already has a save (${summary}). It will be moved to a backup folder, and the save you picked takes its place.`,
  replace: () => "Replace",
  saveImported: () => "Save imported. Rooms you host will use it.",

  // Public rooms.
  publicRooms: () => "Public rooms",
  publicRoomsLoggedOut: () => "Log in to see the public rooms.",
  logIn: () => "Log in",
  publicRoomsLoading: () => "Loading rooms…",
  publicRoomsUnreachable: () => "Can't reach the server right now.",
  publicRoomsNone: () => "No public rooms right now. Host one above!",
  roomPlayers: (players: number, open: number) => `${players}/${open}`,
  noGame: () => "No game",
  yourRoomLabel: () => "Your room",
  dragTooltip: () => "Drag to another slot to move this player.",
  betaLine1: () => "Wii Rollback is in beta.",
  betaLine2Before: () => "Report issues on the ",
  discord: () => "Discord",
  yourRoomTooltip: () => "You're in this room.",
  roomHost: (name: string) => `${name}'s room`,
  needBuild: (name: string, hash: string) => `You don't have this game: ${name ? `${name} ` : ""}(${hash}).`,
  noMatchingGameTitle: () => "You don't have any game matching this hash!",
  noMatchingGame: (name: string, hash: string) =>
    `This room plays ${name || "a game"} (${hash.slice(0, 8)}). Add the exact same game (or mod) to join it.`,
  ok: () => "OK",
  joinPublicTooltip: (build: string) => `Start Dolphin with ${build} and join this room.`,

  // A room's parts (docs/design/brawl-mods.md §6.5): the disc must match, the SD card is synced.
  discPart: (gameId: string | undefined) => (gameId?.startsWith("RSB") ? "Brawl" : "Disc"),
  sdPart: () => "SD card",
  partHash: (part: string, hash: string) => `${part}: ${hash.slice(0, 6)}`,
  discMatches: () => "Your disc matches the room's.",
  discDiffers: (label: string | undefined) =>
    `Brawl ISOs don't match. Make sure you're both on the same revision.${label ? ` The room plays ${label}.` : ""}`,
  discDiffersGame: (label: string | undefined) =>
    `The discs don't match. Make sure you both have the same version of the game.${
      label ? ` The room plays ${label}.` : ""
    }`,
  sdMatches: () => "Your SD card matches the room's.",
  sdDiffers: () => "SD cards do not match. You can download the host's card when you join.",
  sdMissing: () => "The room's game has an SD card, and yours has none. Add the mod's SD card to your game.",
  sdExtra: () => "The room's game has no SD card, and yours has one.",
  fixPartTooltip: () => "Click to change it in your game.",
  noJoinDisc: () => "Wrong disc",

  // Joining with an SD card that differs: asked before Dolphin starts (sd_listing.ts), or by
  // Dolphin (its sd_sync) for a card that only turned out different there.
  sdSyncTitle: () => "Your SD card is different",
  sdAskBody: (mb: string, files: number) =>
    `Your SD card is ${mb} different from the host's (${files} ${files === 1 ? "file" : "files"}). Download it?`,
  sdAskBodyNoFiles: (mb: string) => `Your SD card is ${mb} different from the host's. Download it?`,
  sdAskBodyUnknown: () => "Your SD card is different from the host's. Download theirs?",
  // Said with a difference over SD_BIG_DIFFERENCE_BYTES (Dolphin's chat says it too).
  sdBigDifference: () => "Too big? You might have an entirely different version than the host.",
  // The same, after the question's "Download it?" (the user's wording, 2026-10-19).
  sdBigDifferenceQuestion: () => "If it's too big, you might have an entirely different version than the host.",
  sdSyncBody: (mb: string, files: number, eta: string) =>
    `Your SD card is ${mb} different from the host's (${files} ${
      files === 1 ? "file" : "files"
    }). Download it? ETA: ${eta}`,
  sdSyncWarning: () =>
    "(!) SD cards can be used to run code on your Wii. Only download from a friend you trust, never a stranger.",
  sdSyncFiles: () => "Files that differ:",
  sdSyncMoreFiles: (n: number) => `…and ${n} more`,
  sdSyncNo: () => "No",
  sdSyncYes: () => "Yes",
  sdSyncOverwrite: () => "Yes, and overwrite local card",
  sdSyncChecking: (percent: number) => `Checking your SD card… ${percent}%`,
  sdSyncDownloading: (percent: number, rate: string, eta: string) =>
    `Downloading the host's SD card… ${percent}% · ${rate} · ${eta} left`,
  rate: (bytesPerSecond: number) =>
    bytesPerSecond >= 1024 * 1024
      ? `${(bytesPerSecond / 1024 ** 2).toFixed(1)} MB/s`
      : `${Math.max(1, Math.round(bytesPerSecond / 1024))} KB/s`,
  megabytes: (bytes: number) =>
    bytes >= 1024 * 1024 * 1024
      ? `${(bytes / 1024 ** 3).toFixed(1)} GB`
      : `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`,
  duration: (seconds: number) =>
    seconds < 60
      ? `${Math.max(1, Math.round(seconds))} seconds`
      : `${Math.round(seconds / 60)} ${Math.round(seconds / 60) === 1 ? "minute" : "minutes"}`,
};
