import type { AccountsHttpClient } from "./client";
import { AccountsHttpError } from "./client";
import type { SessionStore } from "./session_store";
import type {
  AccountsMe,
  AccountsPublicUser,
  AccountsResult,
  BugReportRequest,
  BugReportSent,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  MyRulesets,
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

/**
 * Main-process side of the accounts integration: owns the session tokens and
 * turns every call into an `AccountsResult` for the renderer.
 */
export class AccountsManager {
  constructor(private readonly client: AccountsHttpClient, private readonly sessions: SessionStore) {}

  signUp(req: SignUpRequest): Promise<AccountsResult<AccountsMe>> {
    return wrap(async () => {
      const { sessionToken, user } = await this.client.signUp(req);
      await this.sessions.set(user.uid, sessionToken);
      return user;
    });
  }

  login(email: string, password: string): Promise<AccountsResult<AccountsMe>> {
    return wrap(async () => {
      const { sessionToken, user } = await this.client.login(email, password);
      await this.sessions.set(user.uid, sessionToken);
      return user;
    });
  }

  logout(uid: string): Promise<AccountsResult<null>> {
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

  me(uid: string): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.me(token));
  }

  resendVerification(uid: string): Promise<AccountsResult<null>> {
    return this._withSession(uid, async (token) => {
      await this.client.resendVerification(token);
      return null;
    });
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

  publicUser(uid: string): Promise<AccountsResult<AccountsPublicUser>> {
    return wrap(() => this.client.publicUser(uid));
  }

  leaderboard(query: { limit?: number; after?: string }): Promise<AccountsResult<LeaderboardPage>> {
    return wrap(() => this.client.leaderboard(query));
  }

  matchHistory(
    uid: string,
    query: { mode: MatchHistoryFilter; limit?: number; before?: string },
  ): Promise<AccountsResult<MatchHistoryPage>> {
    return this._withSession(uid, (token) => this.client.matchHistory(token, query));
  }

  rooms(uid: string): Promise<AccountsResult<RoomList>> {
    return this._withSession(uid, (token) => this.client.rooms(token));
  }

  setRoomName(uid: string, kind: RoomKind, name: string | null): Promise<AccountsResult<string | null>> {
    return this._withSession(uid, (token) => this.client.setRoomName(token, kind, name));
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
