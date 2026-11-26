// The changelog popup's feed (docs/design/changelog.md): every published changelog, newest first,
// asked for a page at a time (GET /v1/changelogs) as the popup scrolls down.
import type { ChangelogPageQuery } from "@accounts/types";
import log from "electron-log";
import React from "react";

import { changelogHtml } from "./changelog_html";

/** Changelogs asked for at a time: the popup asks for more only while its end is in sight. */
export const FEED_PAGE_SIZE = 2;

export type ChangelogEntry = {
  id: number;
  /** Cleaned (changelogHtml). */
  html: string;
  publishedAt: string;
  everyStart: boolean;
};

export type FeedPage = { entries: ChangelogEntry[]; next: string | null };

export async function fetchFeedPage(query: ChangelogPageQuery): Promise<FeedPage> {
  const accounts = window.electron.accounts;
  // The accounts server this launcher talks to (staging and local ones too): the pictures are there.
  const [page, urls] = await Promise.all([
    accounts.changelogs({ limit: FEED_PAGE_SIZE, ...query }),
    accounts.getServiceUrls(),
  ]);
  return {
    entries: page.changelogs.map((c) => ({
      id: c.id,
      html: changelogHtml(c.html, urls.accountsApi),
      publishedAt: c.publishedAt,
      everyStart: c.everyStart === true,
    })),
    next: page.next,
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
    fetchFeedPage({ after: next })
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
