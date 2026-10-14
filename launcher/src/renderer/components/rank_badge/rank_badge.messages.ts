import type { Rank, RankTier } from "@/services/backend/types";

const TIER_NAMES: Record<RankTier, string> = {
  bronze: "Bronze",
  silver: "Silver",
  gold: "Gold",
  emerald: "Emerald",
  diamond: "Diamond",
  master: "Master",
  legendary: "Legendary",
};

export const RankBadgeMessages = {
  rankName: (rank: Rank) => (rank.division ? `${TIER_NAMES[rank.tier]} ${rank.division}` : TIER_NAMES[rank.tier]),
  setsToPlace: (sets: number) => (sets === 1 ? "1 more set" : `${sets} more sets`),
  setsToPlaceShort: (sets: number) => `${sets} more`,
  placingTooltip: (sets: number) =>
    sets === 1 ? "Placing: 1 more ranked set to get a rank" : `Placing: ${sets} more ranked sets to get a rank`,
};
