import type { AccountsErrorBody, AccountsMe, AccountsPublicUser, SignUpRequest, UserJson } from "./types";

export type SessionResponse = { sessionToken: string; user: AccountsMe };

/** An error answer from the accounts API (or a transport failure, code `network`). */
export class AccountsHttpError extends Error {
  constructor(readonly code: string, message: string, readonly status?: number) {
    super(message);
    this.name = "AccountsHttpError";
  }

  toBody(): AccountsErrorBody {
    return { code: this.code, message: this.message, status: this.status };
  }
}

type FetchLike = typeof fetch;

const REQUEST_TIMEOUT_MS = 15000;

/**
 * Minimal HTTP client for the accounts service. Stateless: callers pass the
 * session token for authenticated endpoints.
 */
export class AccountsHttpClient {
  constructor(
    private readonly baseUrl: string,
    private readonly userAgent: string,
    private readonly fetchImpl: FetchLike = fetch,
  ) {}

  signUp(req: SignUpRequest): Promise<SessionResponse> {
    const body: SignUpRequest = { email: req.email, password: req.password, displayName: req.displayName };
    const invite = req.inviteCode?.trim();
    if (invite) {
      body.inviteCode = invite;
    }
    return this._request("POST", "/v1/auth/signup", { body });
  }

  login(email: string, password: string): Promise<SessionResponse> {
    return this._request("POST", "/v1/auth/login", { body: { email, password } });
  }

  async logout(token: string): Promise<void> {
    await this._request("POST", "/v1/auth/logout", { token });
  }

  me(token: string): Promise<AccountsMe> {
    return this._request("GET", "/v1/me", { token });
  }

  async resendVerification(token: string): Promise<void> {
    await this._request("POST", "/v1/auth/verify-email/resend", { token });
  }

  async requestPasswordReset(email: string): Promise<void> {
    await this._request("POST", "/v1/auth/password-reset/request", { body: { email } });
  }

  initNetplay(token: string, codeStart: string): Promise<AccountsMe> {
    return this._request("POST", "/v1/me/netplay", { token, body: { codeStart } });
  }

  rename(token: string, displayName: string): Promise<AccountsMe> {
    return this._request("POST", "/v1/me/rename", { token, body: { displayName } });
  }

  acceptRules(token: string, num: number): Promise<AccountsMe> {
    return this._request("POST", "/v1/me/accept-rules", { token, body: { num } });
  }

  userJson(token: string): Promise<UserJson> {
    return this._request("GET", "/v1/me/user-json", { token });
  }

  publicUser(uid: string): Promise<AccountsPublicUser> {
    return this._request("GET", `/user/${encodeURIComponent(uid)}?additionalFields=chatMessages,rank`, {});
  }

  private async _request<T>(
    method: "GET" | "POST",
    path: string,
    opts: { token?: string; body?: unknown },
  ): Promise<T> {
    const headers: Record<string, string> = { accept: "application/json", "user-agent": this.userAgent };
    if (opts.token) {
      headers.authorization = `Bearer ${opts.token}`;
    }
    let payload: string | undefined;
    if (opts.body !== undefined) {
      headers["content-type"] = "application/json";
      payload = JSON.stringify(opts.body);
    }

    let res: Response;
    try {
      res = await this.fetchImpl(`${this.baseUrl}${path}`, {
        method,
        headers,
        body: payload,
        signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
      });
    } catch (err) {
      const reason = err instanceof Error ? err.message : String(err);
      throw new AccountsHttpError("network", `Could not reach the account server (${reason}).`);
    }

    const text = await res.text();
    if (!res.ok) {
      throw parseError(res.status, text);
    }
    if (res.status === 204 || text.trim() === "") {
      return undefined as T;
    }
    try {
      return JSON.parse(text) as T;
    } catch {
      throw new AccountsHttpError("bad_response", "The account server sent an unreadable response.", res.status);
    }
  }
}

function parseError(status: number, text: string): AccountsHttpError {
  try {
    const parsed = JSON.parse(text);
    const err = parsed?.error;
    if (err && typeof err.message === "string") {
      return new AccountsHttpError(String(err.code ?? "error"), err.message, status);
    }
  } catch {
    // Not JSON: axum's own rejections (malformed body) are plain text.
  }
  const message = text.trim() !== "" && text.length < 300 ? text.trim() : `Account server error (HTTP ${status}).`;
  return new AccountsHttpError(status === 401 ? "unauthorized" : "error", message, status);
}
