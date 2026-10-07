import type { AccountsMe } from "@accounts/types";
import { Preconditions } from "@common/preconditions";
import multicast from "observable-fns/multicast";
import Subject from "observable-fns/subject";

import { delayAndMaybeError } from "../utils";
import { mapAccountToAuthUser } from "./map_user";
import { createMultiAccountService } from "./multi_account.service.mock";
import type { AuthService, AuthUser, MultiAccountService, SignUpArgs } from "./types";

const SHOULD_ERROR = false;

class MockAuthClient implements AuthService {
  private _userSubject = new Subject<AuthUser | undefined>();
  private _onAuthStateChanged = multicast(this._userSubject);
  private _multiAccountService: MultiAccountService;

  constructor() {
    this._multiAccountService = createMultiAccountService();
    this._multiAccountService.onActiveUserChange((user) => {
      this._userSubject.next(user ? mapAccountToAuthUser(user) : undefined);
    });
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async init(): Promise<AuthUser | undefined> {
    return undefined;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async logout(): Promise<void> {
    const activeAccountId = this._multiAccountService.getActiveAccountId();
    if (activeAccountId) {
      await this._multiAccountService.removeAccount(activeAccountId);
    }
  }

  getCurrentUser(): AuthUser | undefined {
    const user = this._multiAccountService.getActiveUser();
    return user ? mapAccountToAuthUser(user) : undefined;
  }

  getCurrentAccount(): AccountsMe | undefined {
    return this._multiAccountService.getActiveUser() ?? undefined;
  }

  onUserChange(onChange: (user: AuthUser | undefined) => void): () => void {
    const subscription = this._onAuthStateChanged.subscribe(onChange);
    return () => {
      subscription.unsubscribe();
    };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async resetPassword(): Promise<void> {
    // Nothing to send in mock mode
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async login(args: { email: string; password: string }): Promise<AuthUser | undefined> {
    await this._multiAccountService.addAccount(args.email, args.password);
    return this.getCurrentUser();
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async signUp(args: SignUpArgs): Promise<AuthUser | undefined> {
    await this._multiAccountService.signUp(args);
    return this.getCurrentUser();
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async updateDisplayName(displayName: string): Promise<void> {
    const user = this._multiAccountService.getActiveUser();
    Preconditions.checkExists(user, "User is not logged in.");
    this._multiAccountService.setUserRecord({ ...user, displayName });
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async refreshUser(): Promise<void> {
    await this._multiAccountService.refreshActiveUser();
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async sendVerificationEmail(): Promise<void> {
    // Do nothing
  }

  getMultiAccountService(): MultiAccountService {
    return this._multiAccountService;
  }
}

export default function createMockAuthClient(): AuthService {
  return new MockAuthClient();
}
