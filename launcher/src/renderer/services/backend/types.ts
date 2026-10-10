import type { LeaderboardPage, MatchHistoryFilter, MatchHistoryPage, RoomList } from "@accounts/types";
import type { PlayKey } from "@dolphin/types";

export type {
  LeaderboardEntry,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryItem,
  MatchHistoryPage,
  MatchHistoryPlayer,
  PublicRoom,
  RoomList,
  RoomMode,
} from "@accounts/types";

/** The ranked rating: an Elo number on Slippi's scale (server/crates/common/src/ranked.rs). No tiers. */
export type RankedProfile = {
  rating: number;
  /** Rated ranked sets (Slippi's `ratingUpdateCount`). */
  setsPlayed: number;
  /** 1-based leaderboard position; null before the first rated set (or from an older server). */
  position?: number | null;
  /** Players on the leaderboard. */
  rankedPlayers?: number;
};

export type UserData = {
  playKey?: PlayKey;
  rankedNetplayProfile?: RankedProfile;
};

export interface BackendService {
  validateUserId(userId: string): Promise<{ displayName: string; connectCode: string }>;
  fetchUserData(): Promise<UserData | undefined>;
  fetchRankedNetplayProfile(userId: string): Promise<RankedProfile | undefined>;
  /** One leaderboard page; `after` is the previous page's `next`. */
  fetchLeaderboard(after?: string): Promise<LeaderboardPage>;
  /** One page of the logged-in player's matches, newest first; `before` is the previous page's `next`. */
  fetchMatchHistory(mode: MatchHistoryFilter, before?: string): Promise<MatchHistoryPage>;
  /** Players online and the public rooms (Home > Overview); needs a login. */
  fetchRooms(): Promise<RoomList>;
  assertPlayKey(playKey: PlayKey): Promise<void>;
  deletePlayKey(): Promise<void>;
  changeDisplayName(name: string): Promise<void>;
  initializeNetplay(codeStart: string): Promise<void>;
}
