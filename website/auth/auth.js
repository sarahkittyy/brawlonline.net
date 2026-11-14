/* Brawl Online website login: the session every logged-in page of the site shares (the blog
   editor at /blog/write/, later an /admin/ page). It is the launcher's account system: the login
   page (/login/) posts to accounts' /v1/auth/login and keeps the session token in localStorage; the
   pages send it as "Authorization: Bearer <token>", as the launcher does. No cookies, so there is no
   cross-site request to guard against. Nothing on the home page links to /login/.

   A page that needs a login:

     <script src="/auth/auth.js"></script>
     const session = await BrawlAuth.requireLogin();      // goes to /login/?next=<this page> without one
     if (!BrawlAuth.hasRole(session, "admin")) { ... }     // the account's users.role
     const posts = await BrawlAuth.api("/v1/blog/posts");  // JSON in, JSON out, Bearer added
     BrawlAuth.mountAccount(element);                      // "Name (CODE) · Log out"

   docs/design/blog.md §4. */
(function () {
  "use strict";

  const KEY = "brawlonline.session";
  const listeners = new Set();

  class ApiError extends Error {
    constructor(status, code, message, retryAfter) {
      super(message || `Request failed (${status})`);
      this.status = status;
      this.code = code || "";
      this.retryAfter = retryAfter || 0;
    }
  }

  function read() {
    try {
      const s = JSON.parse(localStorage.getItem(KEY) || "null");
      return s && typeof s.token === "string" && s.token ? s : null;
    } catch {
      return null;
    }
  }

  function write(s) {
    if (s) localStorage.setItem(KEY, JSON.stringify(s));
    else localStorage.removeItem(KEY);
    listeners.forEach((cb) => cb(s));
  }

  // Another tab logged in or out.
  window.addEventListener("storage", (e) => {
    if (e.key === KEY) listeners.forEach((cb) => cb(read()));
  });

  /** The parts of accounts' `Me` the site keeps (never the play key). */
  function userOf(me) {
    return {
      uid: me.uid,
      email: me.email,
      displayName: me.displayName,
      connectCode: me.connectCode || "",
      role: me.role || "user",
    };
  }

  async function parse(resp) {
    const type = resp.headers.get("content-type") || "";
    if (resp.status === 204) return null;
    if (type.includes("application/json")) return resp.json();
    return resp.text();
  }

  function errorOf(status, data, headers) {
    const e = data && typeof data === "object" && data.error ? data.error : {};
    const retry = Number(headers && headers.get ? headers.get("retry-after") : 0) || 0;
    let message = e.message;
    if (!message && status === 413) message = "That is too large.";
    if (!message && status >= 500) message = "The server had a problem. Try again in a minute.";
    return new ApiError(status, e.code, message, retry);
  }

  /**
   * Calls the accounts API. `json` is sent as JSON; `body` as is. Throws ApiError on a non-2xx
   * answer. A 401 with a session means it ended (logged out elsewhere, expired): it is dropped.
   */
  async function api(path, { method = "GET", json, body, headers = {}, auth = true } = {}) {
    const h = new Headers(headers);
    const s = read();
    if (auth && s) h.set("Authorization", `Bearer ${s.token}`);
    if (json !== undefined) {
      h.set("Content-Type", "application/json");
      body = JSON.stringify(json);
    }
    let resp;
    try {
      resp = await fetch(path, { method, headers: h, body, credentials: "omit", cache: "no-store" });
    } catch {
      throw new ApiError(0, "network", "Can't reach the server. Check your connection.");
    }
    const data = await parse(resp).catch(() => null);
    if (resp.status === 401 && auth && s) write(null);
    if (!resp.ok) throw errorOf(resp.status, data, resp.headers);
    return data;
  }

  /**
   * Sends a file (or Blob) as the raw body, with upload progress: `onProgress(sent, total)`.
   * Resolves with the JSON answer; rejects with ApiError. `xhr` (optional) gets the request, to
   * abort it.
   */
  function upload(path, file, { method = "PUT", headers = {}, onProgress, xhr: onXhr } = {}) {
    return new Promise((resolve, reject) => {
      const s = read();
      const xhr = new XMLHttpRequest();
      xhr.open(method, path);
      if (s) xhr.setRequestHeader("Authorization", `Bearer ${s.token}`);
      if (file.type) xhr.setRequestHeader("Content-Type", file.type);
      Object.entries(headers).forEach(([k, v]) => xhr.setRequestHeader(k, v));
      xhr.responseType = "text";
      if (onProgress) xhr.upload.onprogress = (e) => onProgress(e.loaded, e.lengthComputable ? e.total : file.size);
      xhr.onerror = () => reject(new ApiError(0, "network", "The upload was cut off. Check your connection."));
      xhr.onabort = () => reject(new ApiError(0, "aborted", "Upload cancelled."));
      xhr.onload = () => {
        let data = null;
        try {
          data = JSON.parse(xhr.responseText);
        } catch {
          data = null;
        }
        if (xhr.status === 401 && s) write(null);
        if (xhr.status >= 200 && xhr.status < 300) resolve(data);
        else reject(errorOf(xhr.status, data, { get: (h) => xhr.getResponseHeader(h) }));
      };
      if (onXhr) onXhr(xhr);
      xhr.send(file);
    });
  }

  async function login(email, password) {
    const data = await api("/v1/auth/login", { method: "POST", json: { email, password }, auth: false });
    const s = { token: data.sessionToken, user: userOf(data.user), since: Date.now() };
    write(s);
    return s;
  }

  async function logout() {
    if (read()) await api("/v1/auth/logout", { method: "POST" }).catch(() => {});
    write(null);
  }

  /** Re-reads the account (its name and role may have changed). Null once the session ended. */
  async function refresh() {
    const s = read();
    if (!s) return null;
    try {
      const me = await api("/v1/me");
      const next = { ...s, user: userOf(me) };
      write(next);
      return next;
    } catch (e) {
      if (e.status === 401 || e.status === 403) {
        write(null);
        return null;
      }
      return s; // offline or a server hiccup: keep it
    }
  }

  /** A path on this site to go back to after logging in; anything else is "/". */
  function safeNext(next) {
    if (typeof next !== "string" || !next.startsWith("/") || next.startsWith("//") || next.includes("\\")) return "/";
    return next;
  }

  function loginUrl(next) {
    const here = next ?? location.pathname + location.search + location.hash;
    return `/login/?next=${encodeURIComponent(safeNext(here))}`;
  }

  /** The session, after checking it with the server; without one, goes to the login page. */
  async function requireLogin() {
    const s = await refresh();
    if (s) return s;
    location.replace(loginUrl());
    return new Promise(() => {}); // the page is leaving
  }

  function hasRole(session, role) {
    return !!session && session.user.role === role;
  }

  /** Calls `cb(session or null)` whenever this or another tab logs in or out. */
  function onChange(cb) {
    listeners.add(cb);
    return () => listeners.delete(cb);
  }

  /** "Name (CODE) · Log out" in `el`. Logging out goes to the login page (back here after). */
  function mountAccount(el, { afterLogout } = {}) {
    const render = (s) => {
      el.replaceChildren();
      el.classList.add("account-chip");
      if (!s) return;
      const who = document.createElement("span");
      who.className = "account-name";
      who.textContent = s.user.displayName;
      if (s.user.connectCode) {
        const code = document.createElement("span");
        code.className = "account-code";
        code.textContent = s.user.connectCode;
        who.append(" ", code);
      }
      const out = document.createElement("button");
      out.type = "button";
      out.className = "pill";
      out.textContent = "Log out";
      out.addEventListener("click", async () => {
        out.disabled = true;
        await logout();
        if (afterLogout) afterLogout();
        else location.assign(loginUrl());
      });
      el.append(who, out);
    };
    render(read());
    return onChange(render);
  }

  window.BrawlAuth = {
    ApiError,
    session: read,
    api,
    upload,
    login,
    logout,
    refresh,
    requireLogin,
    hasRole,
    loginUrl,
    safeNext,
    onChange,
    mountAccount,
  };
})();
