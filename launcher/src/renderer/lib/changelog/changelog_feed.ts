// The changelog popup's feed (docs/design/changelog.md): every published changelog, newest first,
// asked for a page at a time (GET /v1/changelogs) as the popup scrolls down.
import type { ChangelogMention, ChangelogPageQuery } from "@accounts/types";
import { isIconId, parseBadges } from "@common/badges";
import log from "electron-log";
import React from "react";

import { useSettingsStore } from "@/lib/hooks/use_settings";

import { changelogHtml } from "./changelog_html";

/** Changelogs asked for at a time: the popup asks for more only while its end is in sight. */
export const FEED_PAGE_SIZE = 2;

export type ChangelogEntry = {
  id: number;
  /** Cleaned (changelogHtml). */
  html: string;
  publishedAt: string;
  everyStart: boolean;
  /** EmissaryRecomp's patch notes for this release (`v0.7.2`); null for the launcher's. */
  emissaryVersion: string | null;
  /** The players it mentions, by uid: their names go in its `[data-mention]` spans (changelogHtml). */
  mentions: Record<string, ChangelogMention>;
};

const UID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const text = (v: unknown, max: number): string | undefined =>
  typeof v === "string" && v !== "" ? v.slice(0, max) : undefined;

/** The mentioned players as the server sends them, checked: bad entries are dropped. */
export function parseMentions(v: unknown): Record<string, ChangelogMention> {
  const out: Record<string, ChangelogMention> = {};
  if (!Array.isArray(v)) {
    return out;
  }
  for (const m of v.slice(0, 200)) {
    const displayName = text(m?.displayName, 64);
    if (typeof m?.uid !== "string" || !UID.test(m.uid) || !displayName) {
      continue;
    }
    out[m.uid] = {
      uid: m.uid,
      displayName,
      connectCode: text(m.connectCode, 16) ?? "",
      country: text(m.country, 16) ?? null,
      main: text(m.main, 32) ?? null,
      mainIcon: isIconId(m.mainIcon) ? m.mainIcon : null,
      mainIconLabel: isIconId(m.mainIcon) ? text(m.mainIconLabel, 60) ?? null : null,
      badges: parseBadges(m.badges),
    };
  }
  return out;
}

/** `emissary`: the feed has EmissaryRecomp's patch notes too (its next pages are asked for with them). */
export type FeedPage = { entries: ChangelogEntry[]; next: string | null; emissary: boolean };

/** Whether this launcher's feed has EmissaryRecomp's patch notes: Play plays with it. */
export const wantsEmissaryNotes = () => useSettingsStore.getState().settings.playProjectPlusWith === "emissary";

export async function fetchFeedPage(
  query: Omit<ChangelogPageQuery, "emissary"> & { emissary?: boolean },
): Promise<FeedPage> {
  const accounts = window.electron.accounts;
  const { emissary = false, ...rest } = query;
  // The accounts server this launcher talks to (staging and local ones too): the pictures are there.
  const [page, urls] = await Promise.all([
    accounts.changelogs({ limit: FEED_PAGE_SIZE, ...rest, ...(emissary ? { emissary: 1 as const } : {}) }),
    accounts.getServiceUrls(),
  ]);
  return {
    entries: page.changelogs.map((c) => ({
      id: c.id,
      html: changelogHtml(c.html, urls.accountsApi),
      publishedAt: c.publishedAt,
      everyStart: c.everyStart === true,
      emissaryVersion: c.product === "emissary" ? c.version ?? "" : null,
      mentions: parseMentions(c.mentions),
    })),
    next: page.next,
    emissary,
  };
}

/** The feed from its first page on; `loadMore` adds the next page (one request at a time). A failed one stops it. */
export function useChangelogFeed(first: FeedPage) {
  const [feed, setFeed] = React.useState({ first, entries: first.entries, next: first.next, failed: false });
  const loading = React.useRef(false);
  // Opened again with a new first page: start over (a page still on its way is for the old one).
  const current = feed.first === first ? feed : { first, entries: first.entries, next: first.next, failed: false };
  if (current !== feed) {
    setFeed(current);
    loading.current = false;
  }

  const { next, failed } = current;
  const loadMore = React.useCallback(() => {
    if (!next || failed || loading.current) {
      return;
    }
    loading.current = true;
    fetchFeedPage({ after: next, emissary: first.emissary })
      .then((page) =>
        setFeed((f) => {
          if (f.first !== first) {
            return f;
          }
          const have = new Set(f.entries.map((e) => e.id));
          return { ...f, entries: [...f.entries, ...page.entries.filter((e) => !have.has(e.id))], next: page.next };
        }),
      )
      .catch((err) => {
        log.warn(`Changelog: ${err instanceof Error ? err.message : String(err)}`);
        setFeed((f) => (f.first === first ? { ...f, failed: true } : f));
      })
      .finally(() => {
        loading.current = false;
      });
  }, [first, next, failed]);

  return { entries: current.entries, hasMore: next != null && !failed, loadMore };
}
