import type { AccountsMe } from "@accounts/types";
import { Preconditions } from "@common/preconditions";
import log from "electron-log";
import multicast from "observable-fns/multicast";
import Subject from "observable-fns/subject";

import { mapAccountToAuthUser } from "./map_user";
import { createMultiAccountService } from "./multi_account.service";
import type { AuthService, AuthUser, MultiAccountService, SignUpArgs } from "./types";

/**
 * Auth backed by our accounts service (server/crates/accounts), with
 * multi-account support. Replaces Slippi's Firebase auth.
 */
const VERIFICATION_RESEND_INTERVAL_MS = 30000;

class AuthClient implements AuthService {
  private _userSubject = new Subject<AuthUser | undefined>();
  private _onAuthStateChanged = multicast(this._userSubject);
  private _multiAccountService: MultiAccountService;
  /** uid -> when a verification email was last sent (by sign-up or by us). */
  private _verificationSentAt = new Map<string, number>();

  constructor() {
    this._multiAccountService = createMultiAccountService();
  }

  async init(): Promise<AuthUser | undefined> {
    // Initialize multi-account service
    await this._multiAccountService.init();

    // Any change of the active account or its record is a user change
    this._multiAccountService.onActiveUserChange((user) => {
      this._userSubject.next(user ? mapAccountToAuthUser(user) : undefined);
    });

    // Like Firebase's onAuthStateChanged, report the restored state once.
    const current = this.getCurrentUser();
    this._userSubject.next(current);
    return current;
  }

  getMultiAccountService(): MultiAccountService {
    return this._multiAccountService;
  }

  onUserChange(onChange: (user: AuthUser | undefined) => void): () => void {
    const subscription = this._onAuthStateChanged.subscribe(onChange);
    return () => {
      subscription.unsubscribe();
    };
  }

  getCurrentUser(): AuthUser | undefined {
    const user = this._multiAccountService.getActiveUser();
    return user ? mapAccountToAuthUser(user) : undefined;
  }

  getCurrentAccount(): AccountsMe | undefined {
    return this._multiAccountService.getActiveUser() ?? undefined;
  }

  async signUp({ email, displayName, password, inviteCode }: SignUpArgs) {
    await this._multiAccountService.signUp({ email, password, displayName, inviteCode });
    const user = this.getCurrentUser();
    if (user) {
      // Unlike Firebase, our server already sent the verification email at sign-up.
      this._verificationSentAt.set(user.uid, Date.now());
    }
    return user;
  }

  async login({ email, password }: { email: string; password: string }) {
    // Add account via multi-account service (auto-switches if exists)
    await this._multiAccountService.addAccount(email, password);
    return this.getCurrentUser();
  }

  async sendVerificationEmail() {
    const user = this._multiAccountService.getActiveUser();
    Preconditions.checkExists(user, "User is not logged in.");

    if (!user.emailVerified && user.emailVerificationRequired) {
      // The verify step sends one when it opens; do not repeat one sent seconds ago.
      const last = this._verificationSentAt.get(user.uid) ?? 0;
      if (Date.now() - last < VERIFICATION_RESEND_INTERVAL_MS) {
        log.info(`Verification email was sent recently, not sending another`);
        return;
      }
      this._verificationSentAt.set(user.uid, Date.now());
      log.info(`Sending email verification`);
      await window.electron.accounts.resendVerificationEmail(user.uid);
    }
  }

  async refreshUser(): Promise<void> {
    const user = this._multiAccountService.getActiveUser();
    Preconditions.checkExists(user, "User is not logged in.");
    // Notifies listeners through onActiveUserChange
    await this._multiAccountService.refreshActiveUser();
  }

  async logout() {
    const activeAccountId = this._multiAccountService.getActiveAccountId();
    if (activeAccountId) {
      await this._multiAccountService.removeAccount(activeAccountId);
    }
  }

  async resetPassword(email: string) {
    await window.electron.accounts.requestPasswordReset(email.trim());
  }

  async updateDisplayName(displayName: string): Promise<void> {
    const user = this._multiAccountService.getActiveUser();
    Preconditions.checkExists(user, "User is not logged in.");

    const updated = await window.electron.accounts.rename(user.uid, displayName);
    this._multiAccountService.setUserRecord(updated);
  }
}

export default function createAuthClient(): AuthService {
  return new AuthClient();
}
