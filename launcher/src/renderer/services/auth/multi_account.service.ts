/**
 * Multi-Account Service
 *
 * Manages several signed-in accounts (up to 5), like Slippi's launcher did with
 * one Firebase app per account. Sessions are opaque tokens from our accounts
 * service; the main process stores them (encrypted with the OS keychain when
 * available) and makes every API call, so the renderer only ever names an
 * account by its uid.
 *
 * ## Account Switching
 * Switching only changes which stored account is active; every account stays
 * logged in until its session expires or the user logs it out.
 */

import type { AccountsMe } from "@accounts/types";
import { AccountsError, SESSION_EXPIRED_CODES } from "@accounts/types";
import type { AccountData, StoredAccount } from "@settings/types";
import log from "electron-log";
import multicast from "observable-fns/multicast";
import Subject from "observable-fns/subject";

import { generateDisplayPicture } from "@/lib/display_picture";

import type { MultiAccountService, SignUpArgs } from "./types";
import { SessionExpiredError } from "./types";

const MAX_ACCOUNTS_TO_RESTORE = 5;

const accounts = () => window.electron.accounts;

function isSessionExpired(err: unknown): boolean {
  return err instanceof AccountsError && SESSION_EXPIRED_CODES.has(err.code);
}

class MultiAccountClient implements MultiAccountService {
  private _accountsSubject = new Subject<{
    accounts: readonly StoredAccount[];
    activeId: string | null;
  }>();
  private _onAccountsChanged = multicast(this._accountsSubject);
  private _activeUserSubject = new Subject<AccountsMe | null>();
  private _onActiveUserChanged = multicast(this._activeUserSubject);
  private _activeAccountId: string | null = null;
  private _accounts: StoredAccount[] = [];
  private _records = new Map<string, AccountsMe>();
  private _initialized = false;

  /**
   * Initialize the multi-account service and restore previous sessions
   */
  async init(): Promise<void> {
    if (this._initialized) {
      return;
    }

    try {
      this._loadStoredAccounts();
      this._notifyAccountsChanged();

      if (this._activeAccountId) {
        const activeAccount = this._accounts.find((acc) => acc.id === this._activeAccountId);
        if (activeAccount) {
          await this._restoreAccount(activeAccount);
        }
      }

      this._initialized = true;
      log.info("Multi-account service initialized");
    } catch (err) {
      log.error("Failed to initialize multi-account service:", err);
      this._initialized = true;
    }
  }

  /**
   * Sign up a new user and add them to accounts
   */
  async signUp(args: SignUpArgs): Promise<StoredAccount> {
    try {
      const user = await accounts().signUp(args);
      log.info(`Successfully created new user account`);
      return await this._addLoggedInAccount(user, args.email);
    } catch (err) {
      log.error("Failed to sign up new user:", err);
      throw err;
    }
  }

  /**
   * Load stored accounts from settings
   */
  private _loadStoredAccounts(): void {
    try {
      const settings = window.electron.settings.getAppSettingsSync();
      const accountData = settings.accounts;

      if (accountData && Array.isArray(accountData.list)) {
        this._activeAccountId = accountData.activeId;
        this._accounts = accountData.list.map((acc) => ({
          ...acc,
          lastActive: new Date(acc.lastActive),
        }));

        // If the stored list exceeds the limit, keep the most recently active accounts.
        if (this._accounts.length > MAX_ACCOUNTS_TO_RESTORE) {
          log.warn(`Loaded ${this._accounts.length} accounts, capping at ${MAX_ACCOUNTS_TO_RESTORE}`);
          this._accounts.sort((a, b) => b.lastActive.getTime() - a.lastActive.getTime());
          this._accounts = this._accounts.slice(0, MAX_ACCOUNTS_TO_RESTORE);
        }

        // If the stored activeId refers to a truncated account, reset it
        if (this._activeAccountId && !this._accounts.some((acc) => acc.id === this._activeAccountId)) {
          this._activeAccountId = this._accounts[0]?.id ?? null;
        }
      } else {
        if (accountData) {
          log.warn("Invalid accounts data format in settings (list is not an array), resetting");
        }
        this._accounts = [];
        this._activeAccountId = null;
      }

      log.info(`Loaded ${this._accounts.length} stored accounts`);
    } catch (err) {
      log.error("Failed to load stored accounts:", err);
      this._accounts = [];
      this._activeAccountId = null;
    }
  }

  /**
   * Save accounts to settings
   */
  async saveAccounts(): Promise<void> {
    try {
      const accountData: AccountData = {
        activeId: this._activeAccountId,
        list: this._accounts,
      };

      await window.electron.settings.updateSettings([{ key: "accounts", value: accountData }]);

      // Notify listeners of accounts change
      this._notifyAccountsChanged();
    } catch (err) {
      log.error("Failed to save accounts:", err);
      throw err;
    }
  }

  /**
   * Notify listeners that accounts have changed
   */
  private _notifyAccountsChanged(): void {
    this._accountsSubject.next({
      // Deep clone the array and their objects to prevent external modification.
      // e.g. using the array in a zustand store with immer causes the same objects to be frozen since the reference is the same.
      accounts: this._accounts.map((acc) => ({ ...acc, lastActive: new Date(acc.lastActive) })),
      activeId: this._activeAccountId,
    });
  }

  private _notifyActiveUserChanged(): void {
    this._activeUserSubject.next(this.getActiveUser());
  }

  /**
   * Restore the session of a stored account by fetching its record.
   * Returns false if the session is gone and the user has to log in again.
   */
  private async _restoreAccount(account: StoredAccount): Promise<boolean> {
    try {
      const user = await accounts().me(account.id);
      this._records.set(account.id, user);
      this._updateStoredAccount(account, user);
      log.info(`Session restored for account: ${account.displayName}`);
      return true;
    } catch (err) {
      if (isSessionExpired(err)) {
        log.warn(`No active session for account ${account.id} - will need re-authentication`);
        this._records.delete(account.id);
        return false;
      }
      // Offline or server trouble: stay logged in with what we knew, like Firebase's cached user.
      log.warn(`Could not refresh account ${account.id}, using the stored account details:`, err);
      if (!this._records.has(account.id) && (await accounts().hasSession(account.id))) {
        this._records.set(account.id, offlineRecord(account));
      }
      return this._records.has(account.id);
    }
  }

  private _updateStoredAccount(account: StoredAccount, user: AccountsMe) {
    account.email = user.email;
    account.displayName = user.displayName;
  }

  private async _addLoggedInAccount(user: AccountsMe, fallbackEmail: string): Promise<StoredAccount> {
    this._records.set(user.uid, user);
    let account = this._accounts.find((acc) => acc.id === user.uid);
    if (!account) {
      account = mapUserToStoredAccount(user, fallbackEmail);
      this._accounts.push(account);
    } else {
      this._updateStoredAccount(account, user);
    }
    this._activeAccountId = account.id;
    account.lastActive = new Date();
    await this.saveAccounts();
    this._notifyActiveUserChanged();
    return account;
  }

  /**
   * Add a new account (or re-authenticate an existing one)
   */
  async addAccount(email: string, password: string): Promise<StoredAccount> {
    try {
      const user = await accounts().login(email, password);
      const account = await this._addLoggedInAccount(user, email);
      log.info(`Added and switched to account: ${account.displayName}`);
      return account;
    } catch (err) {
      log.error("Failed to add account:", err);
      throw err;
    }
  }

  /**
   * Switch to a different account
   */
  async switchAccount(accountId: string): Promise<void> {
    const account = this._accounts.find((acc) => acc.id === accountId);

    if (!account) {
      throw new Error(`Account ${accountId} not found`);
    }

    // If already active, do nothing
    if (this._activeAccountId === accountId) {
      log.info("Account already active, no switch needed");
      return;
    }

    try {
      const restored = await this._restoreAccount(account);
      if (!restored) {
        // Session expired - throw special error with account info
        throw new SessionExpiredError(account.email, accountId);
      }

      this._activeAccountId = accountId;
      account.lastActive = new Date();
      await this.saveAccounts();
      this._notifyActiveUserChanged();

      log.info(`Switched to account: ${account.displayName}`);
    } catch (err) {
      log.error(`Failed to switch to account ${accountId}:`, err);
      throw err;
    }
  }

  /**
   * Remove an account
   */
  async removeAccount(accountId: string): Promise<void> {
    const accountIndex = this._accounts.findIndex((acc) => acc.id === accountId);

    if (accountIndex === -1) {
      throw new Error(`Account ${accountId} not found`);
    }

    try {
      // End the session first so it cannot be resurrected on next startup
      await accounts().logout(accountId);
      this._records.delete(accountId);

      const removedAccount = this._accounts[accountIndex];
      this._accounts.splice(accountIndex, 1);

      // If this was the active account, switch to another or clear
      if (this._activeAccountId === accountId) {
        this._activeAccountId = null;
        const sortedAccounts = [...this._accounts].sort((a, b) => b.lastActive.getTime() - a.lastActive.getTime());
        for (const next of sortedAccounts) {
          if (await this._restoreAccount(next)) {
            this._activeAccountId = next.id;
            break;
          }
        }
      }

      await this.saveAccounts();
      this._notifyActiveUserChanged();

      log.info(`Removed account: ${removedAccount.displayName}`);
    } catch (err) {
      log.error(`Failed to remove account ${accountId}:`, err);
      throw err;
    }
  }

  getAccounts(): readonly StoredAccount[] {
    // Return sorted by last active (most recent first)
    return [...this._accounts].sort((a, b) => b.lastActive.getTime() - a.lastActive.getTime());
  }

  getActiveAccountId(): string | null {
    return this._activeAccountId;
  }

  getActiveUser(): AccountsMe | null {
    if (!this._activeAccountId) {
      return null;
    }
    return this._records.get(this._activeAccountId) ?? null;
  }

  async refreshActiveUser(): Promise<AccountsMe | null> {
    const id = this._activeAccountId;
    if (!id) {
      return null;
    }
    try {
      const user = await accounts().me(id);
      this.setUserRecord(user);
      return user;
    } catch (err) {
      if (isSessionExpired(err)) {
        this._records.delete(id);
        this._notifyActiveUserChanged();
      }
      throw err;
    }
  }

  setUserRecord(user: AccountsMe): void {
    const previous = this._records.get(user.uid);
    this._records.set(user.uid, user);
    const account = this._accounts.find((acc) => acc.id === user.uid);
    if (account && (account.displayName !== user.displayName || account.email !== user.email)) {
      this._updateStoredAccount(account, user);
      void this.saveAccounts().catch(log.error);
    }
    // Only identity changes are user changes (Slippi's onAuthStateChanged): refreshing the
    // record from the server must not re-trigger the listeners that refresh it.
    if (user.uid === this._activeAccountId && identityChanged(previous, user)) {
      this._notifyActiveUserChanged();
    }
  }

  onAccountsChange(
    onChange: (data: { accounts: readonly StoredAccount[]; activeId: string | null }) => void,
  ): () => void {
    const subscription = this._onAccountsChanged.subscribe(onChange);
    return () => {
      subscription.unsubscribe();
    };
  }

  onActiveUserChange(onChange: (user: AccountsMe | null) => void): () => void {
    const subscription = this._onActiveUserChanged.subscribe(onChange);
    return () => {
      subscription.unsubscribe();
    };
  }
}

function identityChanged(a: AccountsMe | undefined, b: AccountsMe): boolean {
  return (
    !a ||
    a.uid !== b.uid ||
    a.email !== b.email ||
    a.displayName !== b.displayName ||
    a.emailVerified !== b.emailVerified ||
    a.emailVerificationRequired !== b.emailVerificationRequired
  );
}

/** What we show for a logged-in account while the server cannot be reached. */
function offlineRecord(account: StoredAccount): AccountsMe {
  return {
    uid: account.id,
    email: account.email,
    emailVerified: true,
    emailVerificationRequired: false,
    displayName: account.displayName,
    connectCode: null,
    playKey: null,
    latestVersion: "",
    role: "user",
    userJson: null,
  };
}

function mapUserToStoredAccount(user: AccountsMe, defaultEmail: string = ""): StoredAccount {
  return {
    id: user.uid,
    email: user.email || defaultEmail,
    displayName: user.displayName ?? "",
    displayPicture: generateDisplayPicture(user.uid),
    lastActive: new Date(),
  };
}

export function createMultiAccountService(): MultiAccountService {
  return new MultiAccountClient();
}
