export const ProfilePageMessages = {
  profile: () => "Profile",
  rating: () => "Rating",
  setsPlayed: (sets: number) => (sets === 1 ? "1 ranked set" : `${sets} ranked sets`),
  notRankedYet: () => "Not on the leaderboard yet: play a ranked set.",
  matchHistory: () => "Match history",
  all: () => "All",
  ranked: () => "Ranked",
  unranked: () => "Unranked",
  rooms: () => "Rooms",
  mode: () => "Mode",
  date: () => "Date",
  you: () => "You",
  score: () => "Score",
  opponent: () => "Opponents",
  replays: () => "Replays",
  show: (games: number) => (games === 1 ? "Show 1 game" : `Show ${games} games`),
  showHint: (games: number) =>
    games === 1
      ? "Open Replays with this game's replay highlighted"
      : "Open Replays with these games' replays highlighted",
  noReplays: () => "Not on this computer",
  noReplaysHint: () => "Replays are saved by the computer that played the match (Settings > Replays)",
  noMatches: () => "No matches yet.",
  rulesetPage: (code: string) => `Ruleset ${code} on the website`,
};
