/**
 * The player's Discord activity (Settings > Social): Sarah's reviewed strings from the mockups
 * (2026-11-24, run/design/discord-presence), and the launcher's own words for the rest.
 */
export const DiscordMessages = {
  ranked: () => "Ranked",
  unranked: () => "Unranked",
  direct: () => "Direct",
  twos: () => "2v2",
  /** "Ranked · vs Peachy" */
  modeVs: (mode: string, opponents: string) => `${mode} · vs ${opponents}`,
  /** Both opponents of a 2v2: "Lucas & Peachy". */
  pair: (a: string, b: string) => `${a} & ${b}`,
  /** A 2v2 partner: "with Knuckles". */
  withPartner: (name: string) => `with ${name}`,
  game: (n: number) => `Game ${n}`,
  score: (own: number, other: number) => `${own} - ${other}`,
  characterSelect: () => "Character Select",
  stageSelect: () => "Stage Select",
  searching: () => "Searching",
  /** The two players of a game watched: "Peachy vs Knuckles". */
  versus: (a: string, b: string) => `${a} vs ${b}`,
  watching: () => "Watching",
  /** The small pictures' hover texts while watching (the launcher's tab names). */
  spectate: () => "Spectate",
  replays: () => "Replays",
  training: () => "Training",
  projectPunch: () => "Project Punch (Training)",
  otherGames: () => "Other Games",
  inTheLauncher: () => "In the launcher",
  defaultRoomName: (host: string) => `${host}'s room`,
  oneVsOne: () => "1v1",
  ffa: () => "FFA",
  teams: () => "Teams",
  joinTheirRoom: () => "Join their room",
  watchTheirGame: () => "Watch their game",
};
