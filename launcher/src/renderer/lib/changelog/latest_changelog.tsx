// Settings > Changelog: the newest changelog in the same popup as after an update (changelog_dialog.tsx),
// asked for when clicked. Mounted once at the root (app/create.tsx), so it opens over the settings page.
// Closing it stores nothing: the one-time popup after an update still comes as it would have.
import log from "electron-log";
import React from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { ChangelogMessages as Messages } from "./changelog.messages";
import { ChangelogDialogView } from "./changelog_dialog_view";
import { changelogHtml } from "./changelog_html";

type Notify = { showError: (error: unknown) => void; showWarning: (message: string) => void };

export const useLatestChangelog = create(
  combine({ html: null as string | null, open: false, loading: false }, (set, get) => ({
    /** Asks accounts for the newest changelog and opens it; a failure or none at all is a toast. */
    showLatest: (notify: Notify) => {
      if (get().loading) {
        return;
      }
      set({ loading: true });
      const accounts = window.electron.accounts;
      // The accounts server this launcher talks to (staging and local ones too): the pictures are there.
      Promise.all([accounts.latestChangelog(), accounts.getServiceUrls()])
        .then(([latest, urls]) => {
          if (!latest) {
            set({ loading: false });
            notify.showWarning(Messages.none());
            return;
          }
          set({ html: changelogHtml(latest.html, urls.accountsApi), open: true, loading: false });
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
  const html = useLatestChangelog((s) => s.html);
  const open = useLatestChangelog((s) => s.open);
  const close = useLatestChangelog((s) => s.close);
  if (html == null) {
    return null;
  }
  return <ChangelogDialogView open={open} html={html} onClose={close} />;
});
