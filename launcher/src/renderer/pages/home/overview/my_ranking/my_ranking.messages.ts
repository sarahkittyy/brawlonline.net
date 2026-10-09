export const MyRankingMessages = {
  myRanking: () => "My Ranking",
  refresh: () => "Refresh",
  rating: () => "Rating",
  setsPlayed: (sets: number) => (sets === 1 ? "1 ranked set" : `${sets} ranked sets`),
  matchHistory: () => "Match history",
  hide: () => "Hide ranking",
  rankingHiddenNotification: () => "You can re-enable rank display in the settings.",
};
