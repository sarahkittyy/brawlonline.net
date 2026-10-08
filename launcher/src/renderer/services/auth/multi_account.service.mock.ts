import type { AccountsMe } from "@accounts/types";
import type { StoredAccount } from "@settings/types";
import multicast from "observable-fns/multicast";
import Subject from "observable-fns/subject";

import { generateDisplayPicture } from "@/lib/display_picture";

import type { MultiAccountService, SignUpArgs } from "./types";
import { SessionExpiredError } from "./types";

// Log in with test/test or admin/admin in mock mode.
const testUsers = [
  { email: "test", password: "test", displayName: "Test User" },
  { email: "admin", password: "admin", displayName: "Admin User" },
];

function fakeRecord(email: string, displayName: string): AccountsMe {
  return {
    uid: `mock-${email}`,
    email,
    emailVerified: true,
    emailVerificationRequired: true,
    displayName,
    connectCode: null,
    playKey: null,
    latestVersion: "0.0.0",
    role: "user",
    userJson: null,
  };
}

class MockMultiAccountClient implements MultiAccountService {
  private _accountsSubject = new Subject<{ accounts: readonly StoredAccount[]; activeId: string | null }>();
  private _onAccountsChanged = multicast(this._accountsSubject);
  private _userSubject = new Subject<AccountsMe | null>();
  private _onUserChanged = multicast(this._userSubject);
  private _activeAccountId: string | null = null;
  private _accounts: StoredAccount[] = [];
  private _records = new Map<string, AccountsMe>();
  private _passwords = new Map<string, string>(testUsers.map((u) => [u.email, u.password]));
  private _names = new Map<string, string>(testUsers.map((u) => [u.email, u.displayName]));

  async init(): Promise<void> {
    // Nothing to restore in mock mode
  }

  async signUp({ email, password, displayName }: SignUpArgs): Promise<StoredAccount> {
    this._passwords.set(email, password);
    this._names.set(email, displayName);
    return this.addAccount(email, password);
  }

  async addAccount(email: string, password: string): Promise<StoredAccount> {
    if (this._passwords.get(email) !== password) {
      throw new Error("Wrong email or password");
    }
    const record = fakeRecord(email, this._names.get(email) ?? email);
    this._records.set(record.uid, record);
    let account = this._accounts.find((a) => a.id === record.uid);
    if (!account) {
      account = {
        id: record.uid,
        email,
        displayName: record.displayName,
        displayPicture: generateDisplayPicture(record.uid),
        lastActive: new Date(),
      };
      this._accounts.push(account);
    }
    this._activeAccountId = account.id;
    await this.saveAccounts();
    this._userSubject.next(record);
    return account;
  }

  async removeAccount(accountId: string): Promise<void> {
    this._accounts = this._accounts.filter((a) => a.id !== accountId);
    this._records.delete(accountId);
    if (this._activeAccountId === accountId) {
      this._activeAccountId = this._accounts[0]?.id ?? null;
    }
    await this.saveAccounts();
    this._userSubject.next(this.getActiveUser());
  }

  async switchAccount(accountId: string): Promise<void> {
    const account = this._accounts.find((a) => a.id === accountId);
    if (!account) {
      throw new Error(`Account ${accountId} not found`);
    }
    if (!this._records.has(accountId)) {
      throw new SessionExpiredError(account.email, accountId);
    }
    this._activeAccountId = accountId;
    await this.saveAccounts();
    this._userSubject.next(this.getActiveUser());
  }

  getAccounts(): readonly StoredAccount[] {
    return [...this._accounts];
  }

  getActiveAccountId(): string | null {
    return this._activeAccountId;
  }

  async saveAccounts(): Promise<void> {
    this._accountsSubject.next({ accounts: this.getAccounts(), activeId: this._activeAccountId });
  }

  getActiveUser(): AccountsMe | null {
    return this._activeAccountId ? this._records.get(this._activeAccountId) ?? null : null;
  }

  async refreshActiveUser(): Promise<AccountsMe | null> {
    return this.getActiveUser();
  }

  setUserRecord(user: AccountsMe): void {
    this._records.set(user.uid, user);
    this._userSubject.next(this.getActiveUser());
  }

  onAccountsChange(
    onChange: (data: { accounts: readonly StoredAccount[]; activeId: string | null }) => void,
  ): () => void {
    const sub = this._onAccountsChanged.subscribe(onChange);
    return () => sub.unsubscribe();
  }

  onActiveUserChange(onChange: (user: AccountsMe | null) => void): () => void {
    const sub = this._onUserChanged.subscribe(onChange);
    return () => sub.unsubscribe();
  }
}

export function createMultiAccountService(): MultiAccountService {
  return new MockMultiAccountClient();
}
