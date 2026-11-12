import type {
  AdminCodeOwner,
  AdminPlayer,
  AdminUser,
  AdminUserUpdate,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  MyRulesets,
  PlayerProfile,
  ProfileUpdate,
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
  AdminCodeOwner,
  AdminPlayer,
  AdminUser,
  AdminUserUpdate,
  LeaderboardEntry,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryItem,
  MatchHistoryPage,
  MatchHistoryPlayer,
  MyRulesets,
  NameTag,
  PlayerProfile,
  ProfileSocial,
  ProfileUpdate,
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
  SocialPlatform,
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
  /** Ranked sets won and lost (the leaderboard's W-L); absent from an older server. */
  wins?: number;
  losses?: number;
};

export type UserData = {
  playKey?: PlayKey;
  rankedNetplayProfile?: RankedProfile;
  /** An admin account (`users.role = 'admin'`): the launcher's admin tools show. The server checks it again on
   * every admin call. */
  admin?: boolean;
};

export interface BackendService {
  validateUserId(userId: string): Promise<{ displayName: string; connectCode: string }>;
  fetchUserData(): Promise<UserData | undefined>;
  fetchRankedNetplayProfile(userId: string): Promise<RankedProfile | undefined>;
  /** A player's public profile (docs/design/profiles.md) by uid or connect code; anyone's, no login needed. */
  fetchProfile(id: string): Promise<PlayerProfile>;
  /** Changes the logged-in player's mains, country and social media accounts (docs/design/profiles.md). */
  updateProfile(update: ProfileUpdate): Promise<PlayerProfile>;
  /** The page the header's gold trophy opens, or null for no trophy: the server's `TOURNAMENTS_URL`
   * (docs/trophy-button.md). No login needed. */
  fetchTournamentsUrl(): Promise<string | null>;
  /**
   * One leaderboard page; `after` is the previous page's `next`. With `search`, only the players whose
   * name or connect code contains it.
   */
  fetchLeaderboard(after?: string, search?: string): Promise<LeaderboardPage>;
  /** One page of the logged-in player's matches, newest first; `before` is the previous page's `next`. */
  fetchMatchHistory(mode: MatchHistoryFilter, before?: string): Promise<MatchHistoryPage>;
  /** Players online and the public rooms (Home > Overview); needs a login. */
  fetchRooms(): Promise<RoomList>;
  /** Names the rooms of this kind the player hosts, the one they host now too (null: no name;
   * docs/design/rooms.md §1.4). Resolves to what was stored: a slur comes back as a kind name. */
  setRoomName(kind: RoomKind, name: string | null): Promise<string | null>;
  /** Admin accounts only (the server refuses anyone else): accounts with a connect code, the most recently seen
   * or those whose name or code contains `q`. */
  adminPlayers(q?: string): Promise<AdminPlayer[]>;
  adminUser(uid: string): Promise<AdminUser>;
  adminSetMuted(uid: string, muted: boolean): Promise<AdminUser>;
  adminSetBanned(uid: string, banned: boolean): Promise<AdminUser>;
  /** Renames the account, changes its connect code and locks its name; refused (`code_taken`) for a code another
   * account has. */
  adminUpdateUser(uid: string, update: AdminUserUpdate): Promise<AdminUser>;
  adminCodeOwner(code: string): Promise<AdminCodeOwner>;
  /** Renames any open room as its host would; resolves to what was stored. */
  adminRenameRoom(code: string, name: string | null): Promise<string | null>;
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
