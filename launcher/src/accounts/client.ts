import type {
  AccountsErrorBody,
  AccountsMe,
  AccountsPublicUser,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  RoomList,
  SignUpRequest,
  UserJson,
} from "./types";

export type SessionResponse = { sessionToken: string; user: AccountsMe };

/** An error answer from the accounts API (or a transport failure, code `network`). */
export class AccountsHttpError extends Error {
  /** `retryAfter`: seconds, from a 429's `Retry-After` header. */
  constructor(readonly code: string, message: string, readonly status?: number, readonly retryAfter?: number) {
    super(message);
    this.name = "AccountsHttpError";
  }

  toBody(): AccountsErrorBody {
    const body: AccountsErrorBody = { code: this.code, message: this.message, status: this.status };
    if (this.retryAfter !== undefined) {
      body.retryAfter = this.retryAfter;
    }
    return body;
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

  userJson(token: string): Promise<UserJson> {
    return this._request("GET", "/v1/me/user-json", { token });
  }

  publicUser(uid: string): Promise<AccountsPublicUser> {
    return this._request("GET", `/user/${encodeURIComponent(uid)}?additionalFields=chatMessages,rank`, {});
  }

  leaderboard(query: { limit?: number; after?: string }): Promise<LeaderboardPage> {
    return this._request("GET", `/v1/ranked/leaderboard${queryString(query)}`, {});
  }

  matchHistory(
    token: string,
    query: { mode: MatchHistoryFilter; limit?: number; before?: string },
  ): Promise<MatchHistoryPage> {
    return this._request("GET", `/v1/me/matches${queryString(query)}`, { token });
  }

  rooms(token: string): Promise<RoomList> {
    return this._request("GET", "/v1/rooms", { token });
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
      throw parseError(res.status, text, parseRetryAfter(res.headers.get("retry-after")));
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

/** `?a=1&b=x` from the defined, non-empty values (`""` when there are none). */
export function queryString(params: Record<string, string | number | undefined>): string {
  const q = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== "") {
      q.set(key, String(value));
    }
  }
  const s = q.toString();
  return s === "" ? "" : `?${s}`;
}

/** Seconds from a `Retry-After` header (the server sends whole seconds). */
function parseRetryAfter(header: string | null): number | undefined {
  if (header == null || header.trim() === "") {
    return undefined;
  }
  const secs = Number(header.trim());
  return Number.isFinite(secs) && secs >= 0 ? secs : undefined;
}

function parseError(status: number, text: string, retryAfter?: number): AccountsHttpError {
  try {
    const parsed = JSON.parse(text);
    const err = parsed?.error;
    if (err && typeof err.message === "string") {
      return new AccountsHttpError(String(err.code ?? "error"), err.message, status, retryAfter);
    }
  } catch {
    // Not JSON: axum's own rejections (malformed body) are plain text.
  }
  const message = text.trim() !== "" && text.length < 300 ? text.trim() : `Account server error (HTTP ${status}).`;
  return new AccountsHttpError(status === 401 ? "unauthorized" : "error", message, status, retryAfter);
}
