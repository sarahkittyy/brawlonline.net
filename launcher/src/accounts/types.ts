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
  };
};

export type SignUpRequest = {
  email: string;
  password: string;
  displayName: string;
};

/** Error body: `{"error": {"code": "...", "message": "..."}}`. */
export type AccountsErrorBody = { code: string; message: string; status?: number };

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
  /** The resolved service URLs (for display and links). */
  getServiceUrls(): Promise<ServiceUrls>;
}

/** Thrown in the renderer for a failed accounts call; `message` is the server's text. */
export class AccountsError extends Error {
  constructor(readonly code: string, message: string, readonly status?: number) {
    super(message);
    this.name = "AccountsError";
  }
}

/** Codes that mean the stored session is gone and the user has to log in again. */
export const SESSION_EXPIRED_CODES = new Set(["unauthorized", "no_session"]);
