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

/** The `mode` filter of `GET /v1/me/matches`. `unranked` is every mode but Ranked (Unranked and Direct). */
export type MatchHistoryFilter = "all" | "ranked" | "unranked";

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
};

export type MatchHistoryItem = {
  matchId: string;
  mode: "ranked" | "unranked" | "direct";
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

/** `GET /v1/rooms`: players online (games running and logged in) and the public rooms. */
export type RoomList = {
  online: number;
  /** Joinable first, then waiting before in game, then fuller first. */
  rooms: PublicRoom[];
  updatedAt: string;
};

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
