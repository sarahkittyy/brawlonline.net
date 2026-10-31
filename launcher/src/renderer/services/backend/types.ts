import type {
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  MyRulesets,
  PublishRulesetRequest,
  Rank,
  RecentRuleset,
  RoomKind,
  RoomList,
  RulesetDraft,
  RulesetDraftRequest,
  RulesetPage,
  RulesetView,
} from "@accounts/types";
import type { PlayKey } from "@dolphin/types";

export type {
  LeaderboardEntry,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryItem,
  MatchHistoryPage,
  MatchHistoryPlayer,
  MyRulesets,
  PublicModRoom,
  PublicRoom,
  PublishRulesetRequest,
  Rank,
  RankTier,
  RecentRuleset,
  RoomKind,
  RoomList,
  RoomMode,
  RoomRuleset,
  Ruleset,
  RulesetDraft,
  RulesetDraftRequest,
  RulesetPage,
  RulesetView,
  YourRoom,
} from "@accounts/types";

/** The ranked rating: an Elo number on Slippi's scale (server/crates/common/src/ranked.rs), and the rank
 * it gives (docs/design/ranked-tiers.md). */
export type RankedProfile = {
  rating: number;
  /** Rated ranked sets (Slippi's `ratingUpdateCount`). */
  setsPlayed: number;
  /** 1-based leaderboard position; null before the first rated set (or from an older server). */
  position?: number | null;
  /** Players on the leaderboard. */
  rankedPlayers?: number;
  /** The rank held; null while placing (or from an older server). */
  rank?: Rank | null;
  /** Rated sets left before the rank shows; 0 once placed. */
  setsToPlace?: number;
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
  /** Names the rooms of this kind the player hosts, the one they host now too (null: no name;
   * docs/design/rooms.md §1.4). Resolves to what was stored: a slur comes back as a kind name. */
  setRoomName(kind: RoomKind, name: string | null): Promise<string | null>;
  /** A published ruleset by code (anyone's; docs/rulesets-protocol.md). */
  fetchRuleset(code: string): Promise<RulesetView>;
  /** A page of the public list, newest first; `q` searches the name, publisher and code. */
  fetchPublicRulesets(q: string, after?: string): Promise<RulesetPage>;
  /** The logged-in account's drafts and published rulesets. */
  fetchMyRulesets(): Promise<MyRulesets>;
  /** The rulesets the account used lately, newest first. */
  fetchRecentRulesets(): Promise<RecentRuleset[]>;
  publishRuleset(req: PublishRulesetRequest): Promise<RulesetView>;
  setRulesetHidden(code: string, hidden: boolean): Promise<void>;
  /** Saves a draft: a new one without `id`, else replaces it. */
  saveRulesetDraft(req: RulesetDraftRequest, id?: string): Promise<RulesetDraft>;
  deleteRulesetDraft(id: string): Promise<void>;
  /** The remembered ruleset (Home's ruleset box and in-game Create Room); null: P+ default. */
  fetchRememberedRuleset(): Promise<RulesetView | null>;
  setRememberedRuleset(code: string | null): Promise<RulesetView | null>;
  assertPlayKey(playKey: PlayKey): Promise<void>;
  deletePlayKey(): Promise<void>;
  changeDisplayName(name: string): Promise<void>;
  initializeNetplay(codeStart: string): Promise<void>;
}
