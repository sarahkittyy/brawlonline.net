import type { AccountsHttpClient } from "./client";
import { AccountsHttpError } from "./client";
import type { SessionStore } from "./session_store";
import type { AccountsMe, AccountsPublicUser, AccountsResult, SignUpRequest } from "./types";

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

  acceptRules(uid: string, num: number): Promise<AccountsResult<AccountsMe>> {
    return this._withSession(uid, (token) => this.client.acceptRules(token, num));
  }

  publicUser(uid: string): Promise<AccountsResult<AccountsPublicUser>> {
    return wrap(() => this.client.publicUser(uid));
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
