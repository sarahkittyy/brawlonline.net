// The changelog popup, mounted once in app.tsx (docs/design/changelog.md). After an update
// (changelogPending, main/changelog_version.ts) it asks accounts for the feed's first page once, of
// the changelogs published before this launcher was built; it opens when the newest of them is new to
// this launcher (isNewChangelog), and closing it marks that one shown. So a release without patch
// notes still shows the last release's to a launcher that skipped it, and nothing to one that saw
// them. Offline or a failed request: it asks again at the next start. A test-mode launcher (staging)
// asks at every start and shows an every-start changelog (the agent board's "show on staging") each time.
import log from "electron-log";
import React from "react";

import { useSettingsStore } from "@/lib/hooks/use_settings";

import { ChangelogDialogView } from "./changelog_dialog_view";
import type { FeedPage } from "./changelog_feed";
import { fetchFeedPage, useChangelogFeed } from "./changelog_feed";
import { isNewChangelog } from "./changelog_html";

const markShown = (seenAt?: string) =>
  window.electron.settings.updateSettings([
    { key: "changelogPending", value: false },
    ...(seenAt ? [{ key: "changelogSeenAt" as const, value: seenAt }] : []),
  ]);

const testMode = window.electron.bootstrap.testMode === true;

export const ChangelogDialog = React.memo(function ChangelogDialog() {
  // As the page started: whether this start follows an update, and the last one shown.
  const [{ pending, seenAt }] = React.useState(() => {
    const s = useSettingsStore.getState();
    return { pending: s.changelogPending === true, seenAt: s.changelogSeenAt };
  });
  // The first page shown, and what closing it stores (none for staging's every-start one).
  const [shown, setShown] = React.useState<{ first: FeedPage; seenAt?: string } | null>(null);
  const [open, setOpen] = React.useState(false);

  React.useEffect(() => {
    if (!pending && !testMode) {
      return;
    }
    let cancelled = false;
    // One published after this launcher was built is about a release still to come: left out.
    fetchFeedPage({ before: __DATE__ })
      .then((first) => {
        if (cancelled) {
          return;
        }
        const everyStart = testMode && first.entries[0]?.everyStart === true;
        const newest = first.entries.find((e) => !e.everyStart);
        if (!everyStart && !pending) {
          return;
        }
        if (!everyStart && !newest) {
          // None published yet: asked again at the next start (changelogPending stays). The first
          // push with patch notes publishes them only once its server deploy is done, dated
          // before the push, so a launcher updated sooner still shows them at its next start.
          return;
        }
        if (!everyStart && !isNewChangelog(newest!.publishedAt, seenAt, __DATE__)) {
          void markShown();
          return;
        }
        // Staging's every-start one comes back at the next start whatever is stored.
        setShown({ first, seenAt: everyStart ? undefined : newest?.publishedAt });
        setOpen(true);
      })
      .catch((err) => {
        // Asked again at the next start (changelogPending stays).
        log.warn(`Changelog: ${err instanceof Error ? err.message : String(err)}`);
      });
    return () => {
      cancelled = true;
    };
  }, [pending, seenAt]);

  if (!shown) {
    return null;
  }
  return (
    <Feed
      open={open}
      first={shown.first}
      onClose={() => {
        setOpen(false);
        void markShown(shown.seenAt);
      }}
    />
  );
});

/** The popup over a feed from its first page on. */
export const Feed = ({ open, first, onClose }: { open: boolean; first: FeedPage; onClose: () => void }) => {
  const { entries, hasMore, loadMore } = useChangelogFeed(first);
  return (
    <ChangelogDialogView open={open} entries={entries} hasMore={hasMore} onLoadMore={loadMore} onClose={onClose} />
  );
};
