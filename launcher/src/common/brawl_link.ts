/**
 * `brawlonline://` links (docs/rulesets-protocol.md §4). The website's `/r/<code>` page opens
 * `brawlonline://room/create?ruleset=<code>`: the launcher makes the code the account's remembered
 * ruleset and creates a room with it, as Home's Create room does; `&private=1` (the page's CREATE
 * PRIVATE ROOM) makes the room private. The OS hands the link to the
 * launcher on the command line (Windows, Linux; a running launcher gets it as `second-instance`) or
 * as `open-url` (macOS). The Discord activity's buttons (renderer/lib/discord) go through the website
 * (`/?join=<code>`, `/?watch=<stream>`) to `brawlonline://room/join?code=<code>` (the launcher joins
 * the room, as Join their room does) and `brawlonline://watch?stream=<stream>` (it watches that live
 * game on the Spectate tab). Nothing else is acted on; anything else in the scheme is logged and dropped.
 */

import { URL_SCHEME } from "@common/product";
import { parseRulesetCode } from "@common/rulesets";

export type BrawlLink =
  | {
      kind: "create-room";
      /** The ruleset code, normalized; null: the remembered one stays. */
      ruleset: string | null;
      /** `private=1` (or `true`): a private room, joinable by its code only. */
      isPrivate: boolean;
    }
  | {
      kind: "join-room";
      /** 4 letters, no vowels or Y. */
      code: string;
    }
  | {
      kind: "watch";
      /** A /v1/live stream id (12 hex digits). */
      stream: string;
    };

/** Whether a command-line argument (or an open-url) is one of our links. */
export function isBrawlLink(arg: string): boolean {
  return arg.toLowerCase().startsWith(`${URL_SCHEME}:`);
}

/** The first of our links on a command line, if any. */
export function findBrawlLink(argv: readonly string[]): string | undefined {
  return argv.find(isBrawlLink);
}

/** Reads a link; null for anything we don't act on (another path, a bad ruleset code). */
export function parseBrawlLink(link: string): BrawlLink | null {
  let url: URL;
  try {
    url = new URL(link);
  } catch {
    return null;
  }
  if (url.protocol !== `${URL_SCHEME}:`) {
    return null;
  }
  // `brawlonline://room/create`: host "room", path "/create" (a trailing slash is fine).
  const target = `${url.hostname}${url.pathname}`.replace(/\/+$/, "").toLowerCase();
  if (target === "room/join") {
    const code = (url.searchParams.get("code") ?? "").toUpperCase();
    return /^[BCDFGHJKLMNPQRSTVWXZ]{4}$/.test(code) ? { kind: "join-room", code } : null;
  }
  if (target === "watch") {
    const stream = (url.searchParams.get("stream") ?? "").toLowerCase();
    return /^[0-9a-f]{12}$/.test(stream) ? { kind: "watch", stream } : null;
  }
  if (target !== "room/create") {
    return null;
  }
  const priv = (url.searchParams.get("private") ?? "").toLowerCase();
  const isPrivate = priv === "1" || priv === "true";
  const raw = url.searchParams.get("ruleset");
  if (raw === null || raw === "") {
    return { kind: "create-room", ruleset: null, isPrivate };
  }
  const ruleset = parseRulesetCode(raw);
  return ruleset ? { kind: "create-room", ruleset, isPrivate } : null;
}
