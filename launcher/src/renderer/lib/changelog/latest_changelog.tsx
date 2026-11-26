// Settings > Changelog: the feed of every changelog, newest first, in the same popup as after an
// update (changelog_dialog.tsx), asked for when clicked. Mounted once at the root (app/create.tsx), so
// it opens over the settings page. Closing it stores nothing: the one-time popup after an update still
// comes as it would have.
import log from "electron-log";
import React from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { ChangelogMessages as Messages } from "./changelog.messages";
import { Feed } from "./changelog_dialog";
import type { FeedPage } from "./changelog_feed";
import { fetchFeedPage, wantsEmissaryNotes } from "./changelog_feed";

type Notify = { showError: (error: unknown) => void; showWarning: (message: string) => void };

export const useLatestChangelog = create(
  combine({ first: null as FeedPage | null, open: false, loading: false }, (set, get) => ({
    /** Asks accounts for the feed's first page and opens it; a failure or none at all is a toast. */
    showLatest: (notify: Notify) => {
      if (get().loading) {
        return;
      }
      set({ loading: true });
      fetchFeedPage({ emissary: wantsEmissaryNotes() })
        .then((first) => {
          if (first.entries.length === 0) {
            set({ loading: false });
            notify.showWarning(Messages.none());
            return;
          }
          set({ first, open: true, loading: false });
        })
        .catch((err) => {
          set({ loading: false });
          log.warn(`Changelog: ${err instanceof Error ? err.message : String(err)}`);
          notify.showError(err);
        });
    },
    close: () => set({ open: false }),
  })),
);

export const LatestChangelogDialog = React.memo(function LatestChangelogDialog() {
  const first = useLatestChangelog((s) => s.first);
  const open = useLatestChangelog((s) => s.open);
  const close = useLatestChangelog((s) => s.close);
  if (first == null) {
    return null;
  }
  return <Feed open={open} first={first} onClose={close} />;
});
