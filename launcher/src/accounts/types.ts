/**
 * Types for the accounts API (server/crates/accounts, `src/api.rs`).
 *
 * The launcher talks to it from the main process (no CORS, and the session
 * tokens never reach the renderer's storage). Endpoint names mirror the Slippi
 * operations they replace: `createUserNew` -> signup, `signInWithEmailAndPassword`
 * -> login, `getUser` -> me, `userInitNetplay` -> netplay, `userRename` -> rename.
 * Slippi's `userAcceptRules` has no caller: the launcher shows no rules yet, so
 * the server's accept-rules endpoint and `rulesVersion` fields go unused.
 */

import type { ServiceUrls } from "@common/product";
import type { Ruleset } from "@common/rulesets";

export type { Ruleset } from "@common/rulesets";

/** Exactly what Slippi's launcher writes to `user.json` (`src/dolphin/playkey.ts`). */
export type UserJson = {
  uid: string;
  playKey: string;
  connectCode: string;
  displayName: string;
  latestVersion: string;
};

/** `GET /v1/me` */
export type AccountsMe = {
  uid: string;
  email: string;
  emailVerified: boolean;
  emailVerificationRequired: boolean;
  displayName: string;
  connectCode: string | null;
  playKey: string | null;
  latestVersion: string;
  role: string;
  userJson: UserJson | null;
};

/** `GET /user/{uid}?additionalFields=chatMessages,rank` (Slippi's users-rest shape). */
export type AccountsPublicUser = {
  uid: string;
  displayName: string;
  connectCode: string;
  latestVersion: string;
  chatMessages: string[];
  rank: {
    ratingOrdinal: number;
    ratingUpdateCount: number;
    dailyGlobalPlacement: number | null;
    dailyRegionalPlacement: number | null;
    /** 1-based leaderboard position; null before the first rated set. */
    position: number | null;
    /** Players on the leaderboard (at least one rated set). */
    rankedPlayers: number;
  };
};

/** One row of `GET /v1/ranked/leaderboard`. */
export type LeaderboardEntry = {
  /** 1-based; ties (same rating) are ordered by uid and still get their own positions. */
  position: number;
  uid: string;
  displayName: string;
  connectCode: string;
  rating: number;
  setsPlayed: number;
  /** Sets won and lost. */
  wins: number;
  losses: number;
};

/** `GET /v1/ranked/leaderboard?limit=&after=`: `next` is the `after` of the next page, null on the last. */
export type LeaderboardPage = {
  entries: LeaderboardEntry[];
  next: string | null;
  /** Players on the leaderboard. */
  total: number;
};

/** The `mode` filter of `GET /v1/me/matches`. `unranked` is Unranked and Direct; `rooms` is rooms' games. */
export type MatchHistoryFilter = "all" | "ranked" | "unranked" | "rooms";

export type MatchHistoryPlayer = {
  uid: string;
  /** Empty for a deleted account. */
  displayName: string;
  connectCode: string;
  /** Games won in this match. */
  wins: number;
  /** The rating change of a ranked set; null for Unranked and Direct and when the set did not change it. */
  ratingBefore: number | null;
  ratingAfter: number | null;
  ratingChange: number | null;
  /** A room's game: the player's slot (1-4); null for the other modes. */
  port?: number | null;
  /** A room's team battle: 0 red, 1 blue, 2 green. */
  team?: number | null;
  /** A room's game: 1 = first, shared by a tie and by a team; null until the reports agree, and for a player who left. */
  place?: number | null;
};

export type MatchHistoryItem = {
  matchId: string;
  mode: "ranked" | "unranked" | "direct" | "room";
  /** RFC 3339. */
  createdAt: string;
  /** `ASSIGNED` (undecided, and always for Unranked and Direct), `COMPLETE`, `ABANDONED`, `TERMINATED`, `ERROR`, `ORPHANED`. */
  status: string;
  ranked: boolean;
  players: MatchHistoryPlayer[];
  /** The set's winner (ranked only). */
  winner: string | null;
  /** For example `abandoned by <uid>`, or why a set was void. */
  endReason: string | null;
  /** A room's game: the room's code and how it played; null for the other modes. */
  room?: {
    code: string;
    mode: RoomMode;
    /** The published ruleset it played (as it was then) and its page; null for P+'s default, custom rules and older games. */
    ruleset?: { code: string; name: string; version: number; link: string } | null;
  } | null;
};

/** `GET /v1/me/matches?mode=&limit=&before=`: newest first; `next` is the `before` of the next page. */
export type MatchHistoryPage = {
  matches: MatchHistoryItem[];
  next: string | null;
};

/** How a room plays: two open slots are 1v1 whatever the Teams switch says. */
export type RoomMode = "1v1" | "ffa" | "teams";

/** One public room (`GET /v1/rooms`). */
export type PublicRoom = {
  /** 4 letters, no vowels (e.g. `KFQB`). */
  code: string;
  /** The host's display name. */
  host: string;
  /** Players in the room. */
  players: number;
  /** Open slots: the most players the room takes (2-4). */
  openSlots: number;
  mode: RoomMode;
  /** `in-game` while a game is being played. */
  status: "waiting" | "in-game";
  /** Every player's display name, by slot. */
  names: string[];
  /** An open slot is empty (also during a game: the joiner waits for it to end). */
  joinable: boolean;
};

/** `GET /v1/rooms`: players online (games running and logged in), the public rooms and the queues. */
export type RoomList = {
  online: number;
  /** Joinable first, then waiting before in game, then fuller first. */
  rooms: PublicRoom[];
  /** Players searching Unranked and Ranked (Home's Play buttons). Absent from an older server. */
  queues?: { unranked: number; ranked: number };
  updatedAt: string;
};

/** A published ruleset (docs/rulesets-protocol.md §3). */
export type RulesetView = {
  code: string;
  /** With `version` and `by` (the publisher) filled. */
  ruleset: Ruleset;
  hash: string;
  createdAt: string;
  /** The newest later version that is not hidden. */
  newer: { code: string; version: number } | null;
  /** `https://brawlonline.net/r/<code>`: the page to share. */
  link: string;
  /** Only in the account's own lists. */
  hidden?: boolean;
};

/** An unpublished ruleset of the account's. */
export type RulesetDraft = {
  id: string;
  /** The published version it edits. */
  basedOn: string | null;
  ruleset: Ruleset;
  /** Why it can't be published yet, or null. */
  problem: string | null;
  createdAt: string;
  updatedAt: string;
};

/** `GET /v1/rulesets`: a page of the public list (newest first); `next` is the cursor of the page after it. */
export type RulesetPage = { rulesets: RulesetView[]; next: string | null };

/** `GET /v1/me/rulesets`. */
export type MyRulesets = { drafts: RulesetDraft[]; published: RulesetView[] };

/** `GET /v1/me/rulesets/recent`: newest first. */
export type RecentRuleset = RulesetView & { usedAt: string };

/** `POST /v1/rulesets`: a draft, or a ruleset (and the published version it edits). */
export type PublishRulesetRequest = { draftId: string } | { ruleset: Ruleset; basedOn?: string | null };

/** `POST`/`PUT /v1/me/ruleset-drafts`. */
export type RulesetDraftRequest = { ruleset: Ruleset; basedOn?: string | null };

export type SignUpRequest = {
  email: string;
  password: string;
  displayName: string;
};

/** Error body: `{"error": {"code": "...", "message": "..."}}`. */
export type AccountsErrorBody = { code: string; message: string; status?: number; retryAfter?: number };

/**
 * Results cross the IPC boundary as values, not thrown errors, so the renderer
 * can show the server's message without Electron's "Error invoking remote method" prefix.
 */
export type AccountsResult<T> = { ok: true; value: T } | { ok: false; error: AccountsErrorBody };

/** The renderer-facing API exposed through the preload script as `window.electron.accounts`. */
export interface AccountsApi {
  /** Creates the account, stores its session and returns the user. */
  signUp(req: SignUpRequest): Promise<AccountsMe>;
  /** Logs in, stores the session and returns the user. */
  login(email: string, password: string): Promise<AccountsMe>;
  /** Ends the stored session for this account (best effort) and forgets it. */
  logout(uid: string): Promise<void>;
  /** True if a session token is stored for this account. */
  hasSession(uid: string): Promise<boolean>;
  me(uid: string): Promise<AccountsMe>;
  resendVerificationEmail(uid: string): Promise<void>;
  requestPasswordReset(email: string): Promise<void>;
  initNetplay(uid: string, codeStart: string): Promise<AccountsMe>;
  rename(uid: string, displayName: string): Promise<AccountsMe>;
  publicUser(uid: string): Promise<AccountsPublicUser>;
  /** One leaderboard page (public). */
  leaderboard(query: { limit?: number; after?: string }): Promise<LeaderboardPage>;
  /** One page of this account's match history. */
  matchHistory(
    uid: string,
    query: { mode: MatchHistoryFilter; limit?: number; before?: string },
  ): Promise<MatchHistoryPage>;
  /** Players online and the public rooms. */
  rooms(uid: string): Promise<RoomList>;
  /** A published ruleset by code (public). */
  ruleset(code: string): Promise<RulesetView>;
  /** A page of the public list (`q` searches the name, publisher and code). */
  browseRulesets(query: { q?: string; limit?: number; after?: string }): Promise<RulesetPage>;
  myRulesets(uid: string): Promise<MyRulesets>;
  recentRulesets(uid: string): Promise<RecentRuleset[]>;
  publishRuleset(uid: string, req: PublishRulesetRequest): Promise<RulesetView>;
  setRulesetHidden(uid: string, code: string, hidden: boolean): Promise<void>;
  createRulesetDraft(uid: string, req: RulesetDraftRequest): Promise<RulesetDraft>;
  updateRulesetDraft(uid: string, id: string, req: RulesetDraftRequest): Promise<RulesetDraft>;
  deleteRulesetDraft(uid: string, id: string): Promise<void>;
  /** The remembered ruleset (null: P+ default). */
  rememberedRuleset(uid: string): Promise<RulesetView | null>;
  setRememberedRuleset(uid: string, code: string | null): Promise<RulesetView | null>;
  /** The resolved service URLs (for display and links). */
  getServiceUrls(): Promise<ServiceUrls>;
}

/** Thrown in the renderer for a failed accounts call; `message` is the server's text. */
export class AccountsError extends Error {
  /** `retryAfter`: seconds to wait, from a 429's `Retry-After`. */
  constructor(readonly code: string, message: string, readonly status?: number, readonly retryAfter?: number) {
    super(message);
    this.name = "AccountsError";
  }
}

/** Codes that mean the stored session is gone and the user has to log in again. */
export const SESSION_EXPIRED_CODES = new Set(["unauthorized", "no_session"]);
