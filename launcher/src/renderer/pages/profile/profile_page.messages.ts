export const ProfilePageMessages = {
  profile: () => "Profile",
  rating: () => "Rating",
  setsPlayed: (sets: number) => (sets === 1 ? "1 ranked set" : `${sets} ranked sets`),
  notRankedYet: () => "Not on the leaderboard yet: play a ranked set.",
  notPlacedYet: (sets: number) =>
    sets === 1 ? "On the leaderboard after 1 more ranked set." : `On the leaderboard after ${sets} more ranked sets.`,
  ratingHidden: () => "Still placing: the rating shows after their placement sets",
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
  /** Beside the score: the match's games on this computer that added no win (GameEnding). */
  noWinGames: ({
    draw,
    quit,
    desync,
    unfinished,
  }: {
    draw: number;
    quit: number;
    desync: number;
    unfinished: number;
  }) =>
    [
      draw && (draw === 1 ? "1 draw" : `${draw} draws`),
      quit && `${quit} quit, not counted`,
      desync && `${desync} desynced, not counted`,
      unfinished && `${unfinished} unfinished`,
    ]
      .filter(Boolean)
      .join(" · "),
  noWinGamesHint: () =>
    "From the replays on this computer: a game that was quit or desynced did not count, and a draw adds no win.",
  noMatches: () => "No matches yet.",
  rulesetPage: (code: string) => `Ruleset ${code} on the website`,
};
