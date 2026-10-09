export const LeaderboardPageMessages = {
  leaderboard: () => "Leaderboard",
  rankedPlayers: (n: number) => (n === 1 ? "1 ranked player" : `${n} ranked players`),
  position: () => "#",
  player: () => "Player",
  connectCode: () => "Connect code",
  rating: () => "Rating",
  record: () => "W-L",
  empty: () => "Nobody has played a ranked set yet.",
};
