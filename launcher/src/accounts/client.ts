import type {
  AccountsErrorBody,
  AccountsMe,
  AccountsPublicUser,
  BugReportCreated,
  BugReportRequest,
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
/** A bug report's logs zip can be tens of MB on a slow uplink. */
const UPLOAD_TIMEOUT_MS = 5 * 60 * 1000;

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

  /** `PUT /v1/me/room-name`: what was stored. */
  async setRoomName(token: string, kind: RoomKind, name: string | null): Promise<string | null> {
    const res = await this._request<{ kind: RoomKind; name: string | null }>("PUT", "/v1/me/room-name", {
      token,
      body: { kind, name },
    });
    return res.name;
  }

  /** The Other Games room with this exact code, private ones too (which game it needs). */
  modRoom(token: string, code: string): Promise<PublicModRoom> {
    return this._request("GET", `/v1/mod-rooms/${encodeURIComponent(code)}`, { token });
  }

  ruleset(code: string): Promise<RulesetView> {
    return this._request("GET", `/v1/rulesets/${encodeURIComponent(code)}`, {});
  }

  browseRulesets(query: { q?: string; limit?: number; after?: string }): Promise<RulesetPage> {
    return this._request("GET", `/v1/rulesets${queryString(query)}`, {});
  }

  myRulesets(token: string): Promise<MyRulesets> {
    return this._request("GET", "/v1/me/rulesets", { token });
  }

  async recentRulesets(token: string): Promise<RecentRuleset[]> {
    const res = await this._request<{ rulesets: RecentRuleset[] }>("GET", "/v1/me/rulesets/recent", { token });
    return res.rulesets;
  }

  publishRuleset(token: string, req: PublishRulesetRequest): Promise<RulesetView> {
    return this._request("POST", "/v1/rulesets", { token, body: req });
  }

  async setRulesetHidden(token: string, code: string, hidden: boolean): Promise<void> {
    await this._request("PUT", `/v1/me/rulesets/${encodeURIComponent(code)}/hidden`, { token, body: { hidden } });
  }

  createRulesetDraft(token: string, req: RulesetDraftRequest): Promise<RulesetDraft> {
    return this._request("POST", "/v1/me/ruleset-drafts", { token, body: req });
  }

  updateRulesetDraft(token: string, id: string, req: RulesetDraftRequest): Promise<RulesetDraft> {
    return this._request("PUT", `/v1/me/ruleset-drafts/${encodeURIComponent(id)}`, { token, body: req });
  }

  async deleteRulesetDraft(token: string, id: string): Promise<void> {
    await this._request("DELETE", `/v1/me/ruleset-drafts/${encodeURIComponent(id)}`, { token });
  }

  async rememberedRuleset(token: string): Promise<RulesetView | null> {
    const res = await this._request<{ ruleset: RulesetView | null }>("GET", "/v1/me/ruleset", { token });
    return res.ruleset;
  }

  async setRememberedRuleset(token: string, code: string | null): Promise<RulesetView | null> {
    const res = await this._request<{ ruleset: RulesetView | null }>("PUT", "/v1/me/ruleset", {
      token,
      body: { code },
    });
    return res.ruleset;
  }

  /** A bug report's text. With a session token it carries the account; without one it is anonymous. */
  createBugReport(token: string | undefined, req: BugReportRequest): Promise<BugReportCreated> {
    return this._request("POST", "/v1/bug-reports", { token, body: req });
  }

  /** The report's logs zip, with the token `createBugReport` answered (once, within an hour). */
  async uploadBugReportLogs(id: number, uploadToken: string, zip: Uint8Array<ArrayBuffer>): Promise<void> {
    await this._request("PUT", `/v1/bug-reports/${id}/logs`, {
      raw: { data: zip, contentType: "application/zip" },
      headers: { "x-upload-token": uploadToken },
      timeoutMs: UPLOAD_TIMEOUT_MS,
    });
  }

  /** An Other Games room's SD card listing (accounts `sd_cards.rs`): the host's launcher, when it
   * hosts a game with a card, so joiners see what differs before Dolphin starts. */
  async uploadSdListing(token: string, root: string, listing: Uint8Array<ArrayBuffer>): Promise<void> {
    await this._request("PUT", `/v1/sd-cards/${encodeURIComponent(root)}/listing`, {
      token,
      raw: { data: listing, contentType: "application/octet-stream" },
      timeoutMs: UPLOAD_TIMEOUT_MS,
    });
  }

  /** The listing of the room card `root` (a 404 `AccountsHttpError` when the server has none). */
  sdListing(token: string, root: string): Promise<Uint8Array> {
    return this._request("GET", `/v1/sd-cards/${encodeURIComponent(root)}/listing`, {
      token,
      binary: true,
      timeoutMs: UPLOAD_TIMEOUT_MS,
    });
  }

  private async _request<T>(
    method: "GET" | "POST" | "PUT" | "DELETE",
    path: string,
    opts: {
      token?: string;
      body?: unknown;
      raw?: { data: Uint8Array<ArrayBuffer>; contentType: string };
      headers?: Record<string, string>;
      timeoutMs?: number;
      /** The answer's bytes (a `Uint8Array`) instead of its JSON. */
      binary?: boolean;
    },
  ): Promise<T> {
    const headers: Record<string, string> = {
      accept: "application/json",
      "user-agent": this.userAgent,
      ...opts.headers,
    };
    if (opts.token) {
      headers.authorization = `Bearer ${opts.token}`;
    }
    let payload: string | Uint8Array<ArrayBuffer> | undefined;
    if (opts.body !== undefined) {
      headers["content-type"] = "application/json";
      payload = JSON.stringify(opts.body);
    } else if (opts.raw) {
      headers["content-type"] = opts.raw.contentType;
      payload = opts.raw.data;
    }

    let res: Response;
    try {
      res = await this.fetchImpl(`${this.baseUrl}${path}`, {
        method,
        headers,
        body: payload,
        signal: AbortSignal.timeout(opts.timeoutMs ?? REQUEST_TIMEOUT_MS),
      });
    } catch (err) {
      const reason = err instanceof Error ? err.message : String(err);
      throw new AccountsHttpError("network", `Could not reach the account server (${reason}).`);
    }

    if (opts.binary && res.ok) {
      return new Uint8Array(await res.arrayBuffer()) as T;
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
