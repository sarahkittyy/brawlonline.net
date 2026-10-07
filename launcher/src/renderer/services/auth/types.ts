// Multi-account types
// Note: StoredAccount and AccountData are defined in @settings/types.ts
import type { AccountsMe } from "@accounts/types";
import type { StoredAccount } from "@settings/types";

export type AuthUser = {
  uid: string;
  displayName: string;
  displayPicture: string;
  email: string;
  emailVerified: boolean;
};

export class SessionExpiredError extends Error {
  constructor(public email: string, public accountId: string) {
    super(`Session expired for ${accountId}`);
    this.name = "SessionExpiredError";
  }
}

export type SignUpArgs = { email: string; password: string; displayName: string; inviteCode?: string };

export interface AuthService {
  getCurrentUser(): AuthUser | undefined;
  /** The full account record from the accounts API for the active account (cached). */
  getCurrentAccount(): AccountsMe | undefined;
  init(): Promise<AuthUser | undefined>;
  login(args: { email: string; password: string }): Promise<AuthUser | undefined>;
  logout(): Promise<void>;
  refreshUser(): Promise<void>;
  sendVerificationEmail(): Promise<void>;
  onUserChange(onChange: (user: AuthUser | undefined) => void): () => void;
  resetPassword(email: string): Promise<void>;
  signUp(args: SignUpArgs): Promise<AuthUser | undefined>;
  updateDisplayName(displayName: string): Promise<void>;
  // Multi-account support
  getMultiAccountService(): MultiAccountService;
}

export interface MultiAccountService {
  // Initialization
  init(): Promise<void>;

  // Account Management
  signUp(args: SignUpArgs): Promise<StoredAccount>;
  addAccount(email: string, password: string): Promise<StoredAccount>;
  removeAccount(accountId: string): Promise<void>;
  switchAccount(accountId: string): Promise<void>;
  getAccounts(): readonly StoredAccount[];
  getActiveAccountId(): string | null;
  saveAccounts(): Promise<void>;

  /** The last known account record of the active account, or null when logged out. */
  getActiveUser(): AccountsMe | null;
  /** Re-fetches the active account record from the server. */
  refreshActiveUser(): Promise<AccountsMe | null>;
  /** Replaces the cached record of an account (after a mutation returned a fresh one). */
  setUserRecord(user: AccountsMe): void;

  // Notifications
  onAccountsChange(
    onChange: (data: { accounts: readonly StoredAccount[]; activeId: string | null }) => void,
  ): () => void;
  onActiveUserChange(onChange: (user: AccountsMe | null) => void): () => void;
}
