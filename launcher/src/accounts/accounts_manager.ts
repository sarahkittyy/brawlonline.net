import type { AccountsHttpClient } from "./client";
import { AccountsHttpError } from "./client";
import type { SessionStore } from "./session_store";
import type {
  AccountsMe,
  AccountsPublicUser,
  AccountsResult,
  AdminCodeOwner,
  AdminPlayer,
  AdminUser,
  AdminUserUpdate,
  BugReportRequest,
  BugReportSent,
  LauncherSettings,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  MatchOpponent,
  MyRulesets,
  PlayerProfile,
  ProfileUpdate,
  PublicModRoom,
  PublishRulesetRequest,
  RecentRuleset,
  RoomKind,
  RoomList,
  RulesetDraft,
  RulesetDraftRequest,
  RulesetPage,
  RulesetView,
  SignUpRequest,
} from "./types";

/** How long the answers asked for at start (prefetchAtStart) wait for the page's first call. */
export const START_ANSWER_MS = 30_000;

type StartAnswers = {
  uid: string;
  at: number;
  me: Promise<AccountsResult<AccountsMe>> | null;
  publicUser: Promise<AccountsResult<AccountsPublicUser>> | null;
};

/**
 * Main-process side of the accounts integration: owns the session tokens and
 * turns every call into an `AccountsResult` for the renderer.
 */
export class AccountsManager {
  private start: StartAnswers | null = null;

  constructor(
    private readonly client: AccountsHttpClient,
    private readonly sessions: SessionStore,
    private readonly now: () => number = Date.now,
  ) {}

  /**
   * Asks the server for what the page's start waits for before it shows anything (the active
   * account's record, then its ranked profile), as soon as the app is ready: the page asks about
   * half a second later on a fast PC, once its own start is done, and then waited for both, one
   * after the other, the first over a new connection. Each answer goes to the page's first call for
   * it within START_ANSWER_MS, if the server gave it (after a network error the call asks again).
   */
  prefetchAtStart(uid: string): void {
    this.start = { uid, at: this.now(), me: this._me(uid), publicUser: this._publicUser(uid) };
  }

  /** The start's answer of `kind` for `uid`, once, if it is still fresh. */
  private takeStart<K extends "me" | "publicUser">(uid: string, kind: K): StartAnswers[K] {
    const start = this.start;
    if (!start || start.uid !== uid || this.now() - start.at > START_ANSWER_MS) {
      return null;
    }
    const answer = start[kind];
    start[kind] = null;
    return answer;
  }

  signUp(req: SignUpRequest): Promise<AccountsResult<AccountsMe>> {
    this.start = null;
    return wrap(async () => {
      const { sessionToken, user } = await this.client.signUp(req);
      await this.sessions.set(user.uid, sessionToken);
      return user;
    });
  }

  login(email: string, password: string): Promise<AccountsResult<AccountsMe>> {
    this.start = null;
    return wrap(async () => {
      const { sessionToken, user } = await this.client.login(email, password);
      await this.sessions.set(user.uid, sessionToken);
      return user;
    });
  }

  logout(uid: string): Promise<AccountsResult<null>> {
    this.start = null;
    return wrap(async () => {
      const token = await this.sessions.get(uid);
      await this.sessions.delete(uid);
      if (token) {
        // Best effort: the local session is already gone even if the server is unreachable.
        await this.client.logout(token).catch(() => undefined);
      }
      return null;
    });
  }

  hasSession(uid: string): Promise<AccountsResult<boolean>> {
    return wrap(async () => Boolean(await this.sessions.get(uid)));
  }

  async me(uid: string): Promise<AccountsResult<AccountsMe>> {
    const early = await this.takeStart(uid, "me");
    return early && serverAnswered(early) ? early : this._me(uid);
  }

  private _me(uid: string): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.me(token));
  }

  resendVerification(uid: string): Promise<AccountsResult<null>> {
    return this._withSession(uid, async (token) => {
      await this.client.resendVerification(token);
      return null;
    });
  }

  changeEmail(uid: string, email: string): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.changeEmail(token, email));
  }

  requestPasswordReset(email: string): Promise<AccountsResult<null>> {
    return wrap(async () => {
      await this.client.requestPasswordReset(email);
      return null;
    });
  }

  initNetplay(uid: string, codeStart: string): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.initNetplay(token, codeStart));
  }

  rename(uid: string, displayName: string): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.rename(token, displayName));
  }

  async publicUser(uid: string): Promise<AccountsResult<AccountsPublicUser>> {
    const early = await this.takeStart(uid, "publicUser");
    return early && serverAnswered(early) ? early : this._publicUser(uid);
  }

  private _publicUser(uid: string): Promise<AccountsResult<AccountsPublicUser>> {
    return wrap(() => this.client.publicUser(uid));
  }

  updateProfile(uid: string, update: ProfileUpdate): Promise<AccountsResult<PlayerProfile>> {
    return this._withSession(uid, (token) => this.client.updateProfile(token, update));
  }

  profile(id: string): Promise<AccountsResult<PlayerProfile>> {
    return wrap(() => this.client.profile(id));
  }

  launcherSettings(): Promise<AccountsResult<LauncherSettings>> {
    return wrap(() => this.client.launcherSettings());
  }

  leaderboard(query: { limit?: number; after?: string; q?: string }): Promise<AccountsResult<LeaderboardPage>> {
    return wrap(() => this.client.leaderboard(query));
  }

  matchHistory(
    uid: string,
    query: { mode: MatchHistoryFilter; limit?: number; before?: string; opponent?: string },
  ): Promise<AccountsResult<MatchHistoryPage>> {
    return this._withSession(uid, (token) => this.client.matchHistory(token, query));
  }

  opponents(uid: string, q: string): Promise<AccountsResult<MatchOpponent[]>> {
    return this._withSession(uid, (token) => this.client.opponents(token, q));
  }

  friendsLeaderboard(uid: string): Promise<AccountsResult<LeaderboardPage>> {
    return this._withSession(uid, (token) => this.client.friendsLeaderboard(token));
  }

  rooms(uid: string): Promise<AccountsResult<RoomList>> {
    return this._withSession(uid, (token) => this.client.rooms(token));
  }

  setRoomName(uid: string, kind: RoomKind, name: string | null): Promise<AccountsResult<string | null>> {
    return this._withSession(uid, (token) => this.client.setRoomName(token, kind, name));
  }

  adminPlayers(uid: string, q?: string): Promise<AccountsResult<AdminPlayer[]>> {
    return this._withSession(uid, (token) => this.client.adminPlayers(token, q));
  }

  adminUser(uid: string, target: string): Promise<AccountsResult<AdminUser>> {
    return this._withSession(uid, (token) => this.client.adminUser(token, target));
  }

  adminSetMuted(uid: string, target: string, muted: boolean): Promise<AccountsResult<AdminUser>> {
    return this._withSession(uid, (token) => this.client.adminSetMuted(token, target, muted));
  }

  adminSetBanned(uid: string, target: string, banned: boolean): Promise<AccountsResult<AdminUser>> {
    return this._withSession(uid, (token) => this.client.adminSetBanned(token, target, banned));
  }

  adminUpdateUser(uid: string, target: string, update: AdminUserUpdate): Promise<AccountsResult<AdminUser>> {
    return this._withSession(uid, (token) => this.client.adminUpdateUser(token, target, update));
  }

  adminCodeOwner(uid: string, code: string): Promise<AccountsResult<AdminCodeOwner>> {
    return this._withSession(uid, (token) => this.client.adminCodeOwner(token, code));
  }

  adminRenameRoom(uid: string, code: string, name: string | null): Promise<AccountsResult<string | null>> {
    return this._withSession(uid, (token) => this.client.adminRenameRoom(token, code, name));
  }

  modRoom(uid: string, code: string): Promise<AccountsResult<PublicModRoom>> {
    return this._withSession(uid, (token) => this.client.modRoom(token, code));
  }

  /** An Other Games room's SD card listing, put on the server by its host (dolphin/mods/sd_listing.ts). */
  uploadSdListing(uid: string, root: string, listing: Uint8Array<ArrayBuffer>): Promise<AccountsResult<void>> {
    return this._withSession(uid, (token) => this.client.uploadSdListing(token, root, listing));
  }

  /** A room card's listing; an error with status 404 when the server has none. */
  sdListing(uid: string, root: string): Promise<AccountsResult<Uint8Array>> {
    return this._withSession(uid, (token) => this.client.sdListing(token, root));
  }

  ruleset(code: string): Promise<AccountsResult<RulesetView>> {
    return wrap(() => this.client.ruleset(code));
  }

  browseRulesets(query: { q?: string; limit?: number; after?: string }): Promise<AccountsResult<RulesetPage>> {
    return wrap(() => this.client.browseRulesets(query));
  }

  myRulesets(uid: string): Promise<AccountsResult<MyRulesets>> {
    return this._withSession(uid, (token) => this.client.myRulesets(token));
  }

  recentRulesets(uid: string): Promise<AccountsResult<RecentRuleset[]>> {
    return this._withSession(uid, (token) => this.client.recentRulesets(token));
  }

  publishRuleset(uid: string, req: PublishRulesetRequest): Promise<AccountsResult<RulesetView>> {
    return this._withSession(uid, (token) => this.client.publishRuleset(token, req));
  }

  setRulesetHidden(uid: string, code: string, hidden: boolean): Promise<AccountsResult<null>> {
    return this._withSession(uid, async (token) => {
      await this.client.setRulesetHidden(token, code, hidden);
      return null;
    });
  }

  createRulesetDraft(uid: string, req: RulesetDraftRequest): Promise<AccountsResult<RulesetDraft>> {
    return this._withSession(uid, (token) => this.client.createRulesetDraft(token, req));
  }

  updateRulesetDraft(uid: string, id: string, req: RulesetDraftRequest): Promise<AccountsResult<RulesetDraft>> {
    return this._withSession(uid, (token) => this.client.updateRulesetDraft(token, id, req));
  }

  deleteRulesetDraft(uid: string, id: string): Promise<AccountsResult<null>> {
    return this._withSession(uid, async (token) => {
      await this.client.deleteRulesetDraft(token, id);
      return null;
    });
  }

  rememberedRuleset(uid: string): Promise<AccountsResult<RulesetView | null>> {
    return this._withSession(uid, (token) => this.client.rememberedRuleset(token));
  }

  setRememberedRuleset(uid: string, code: string | null): Promise<AccountsResult<RulesetView | null>> {
    return this._withSession(uid, (token) => this.client.setRememberedRuleset(token, code));
  }

  /**
   * A bug report: its text first (with `uid`'s session when there is one; logged out it goes
   * without an account, since logging in may be what is broken), then the logs zip `logs` makes.
   * The text is kept when the zip fails, and the answer says why it did.
   */
  sendBugReport(
    uid: string | null,
    req: BugReportRequest,
    logs: () => Promise<Uint8Array<ArrayBuffer>>,
  ): Promise<AccountsResult<BugReportSent>> {
    return wrap(async () => {
      const token = uid ? await this.sessions.get(uid) : undefined;
      const { id, uploadToken } = await this.client.createBugReport(token, req);
      try {
        await this.client.uploadBugReportLogs(id, uploadToken, await logs());
        return { id, logsSent: true };
      } catch (err) {
        return { id, logsSent: false, logsError: err instanceof Error ? err.message : String(err) };
      }
    });
  }

  /** The account's session token (for the social stream, which is not a JSON call). */
  sessionToken(uid: string): Promise<string | undefined> {
    return this.sessions.get(uid);
  }

  private _withSession<T>(uid: string, fn: (token: string) => Promise<T>): Promise<AccountsResult<T>> {
    return wrap(async () => {
      const token = await this.sessions.get(uid);
      if (!token) {
        throw new AccountsHttpError("no_session", "Your session has expired. Please log in again.", 401);
      }
      try {
        return await fn(token);
      } catch (err) {
        if (err instanceof AccountsHttpError && err.status === 401) {
          // Expired or revoked (password change, ban): forget it so the UI asks for a login.
          await this.sessions.delete(uid);
        }
        throw err;
      }
    });
  }
}

/**
 * A settled answer: a value, or an error that asking again would not change (no stored session, an
 * expired one, a 4xx). Not a request that never reached the server or a 5xx, which may pass.
 */
function serverAnswered(result: AccountsResult<unknown>): boolean {
  if (result.ok || result.error.code === "no_session") {
    return true;
  }
  const status = result.error.status;
  return status !== undefined && status >= 400 && status < 500;
}

async function wrap<T>(fn: () => Promise<T>): Promise<AccountsResult<T>> {
  try {
    return { ok: true, value: await fn() };
  } catch (err) {
    if (err instanceof AccountsHttpError) {
      return { ok: false, error: err.toBody() };
    }
    const message = err instanceof Error ? err.message : String(err);
    return { ok: false, error: { code: "internal", message } };
  }
}
