/**
 * The player's Discord activity (Settings > Social): the renderer builds it from what the launcher
 * knows (renderer/lib/discord), the main process sends it to the Discord app on this computer
 * (discord_rpc.ts). Discord shows it as "Brawl Online", the name of the Discord application below.
 */

/** The Discord application whose name ("Brawl Online") the activity shows under. */
export const DISCORD_APPLICATION_ID = "1574665391597162608";

export type DiscordActivity = {
  /** The first line under "Brawl Online" (Ranked · vs Peachy); the member list shows it too. */
  details?: string;
  /** The second line (Game 2 · 1 - 0). */
  state?: string;
  /** What the timer counts from (Unix ms). */
  startedAt?: number;
  /** The big picture: an image URL. */
  largeImage: string;
  /** Its hover text (the stage). */
  largeText?: string;
  /** The small picture: an image URL. */
  smallImage?: string;
  smallText?: string;
  /** "(3 of 4)" after the second line. */
  party?: { size: number; max: number };
  /** Up to 2 buttons, each a web link. */
  buttons?: { label: string; url: string }[];
};

/** A player of the game being played, from game-status.json's `game` (the replay's INFO). */
export type DiscordGamePlayer = {
  port: number;
  uid: string;
  displayName: string;
  connectCode: string;
  characterId: number;
  teamId: number | null;
};

/** The online game Dolphin is in the middle of (game-status.json `game`, only while it is played). */
export type DiscordGame = {
  mode: string;
  gameIndex: number;
  stageId: number | null;
  isTeams: boolean;
  startTime: string | null;
  players: DiscordGamePlayer[];
  /** The player's own port (1-4). */
  ownPort: number | null;
};

/** A replay the playback Dolphin plays. */
export type DiscordReplay = {
  stageId: number | null;
  players: { displayName: string; characterId: number; teamId: number | null }[];
};

/** What the Dolphins the launcher started show, for the activity. */
export type DiscordGameState = {
  /** The netplay Dolphin is open. */
  netplayOpen: boolean;
  /** It plays a game other than Project+ (a Brawl Mods build's name, or "Project Punch"). */
  otherGame: string | null;
  /** game-status.json's `screen` (menus, online-css, room, searching, match, single-player, other), when fresh. */
  screen: string | null;
  /** game-status.json's `scene` (scSelCharacter, scSelStage, scMelee, ...). */
  scene: string | null;
  game: DiscordGame | null;
  /** The playback Dolphin is open. */
  playbackOpen: boolean;
  /** The replay it plays (not a spectated game: the renderer has those). */
  replay: DiscordReplay | null;
};
