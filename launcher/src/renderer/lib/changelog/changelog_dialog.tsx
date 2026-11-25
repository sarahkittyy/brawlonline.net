// The changelog popup, mounted once in app.tsx (docs/design/changelog.md). After an update
// (changelogPending, main/changelog_version.ts) it asks accounts for the newest published changelog
// once; it opens when that one is new to this launcher (isNewChangelog), and closing it marks it shown.
// Offline or a failed request: it asks again at the next start.
import log from "electron-log";
import React from "react";

import { useSettingsStore } from "@/lib/hooks/use_settings";

import { ChangelogDialogView } from "./changelog_dialog_view";
import { changelogHtml, isNewChangelog } from "./changelog_html";

type Shown = { html: string; publishedAt: string };

const markShown = (seenAt?: string) =>
  window.electron.settings.updateSettings([
    { key: "changelogPending", value: false },
    ...(seenAt ? [{ key: "changelogSeenAt" as const, value: seenAt }] : []),
  ]);

export const ChangelogDialog = React.memo(function ChangelogDialog() {
  // As the page started: whether this start follows an update, and the last one shown.
  const [{ pending, seenAt }] = React.useState(() => {
    const s = useSettingsStore.getState();
    return { pending: s.changelogPending === true, seenAt: s.changelogSeenAt };
  });
  const [shown, setShown] = React.useState<Shown | null>(null);
  const [open, setOpen] = React.useState(false);

  React.useEffect(() => {
    if (!pending) {
      return;
    }
    let cancelled = false;
    const accounts = window.electron.accounts;
    // The accounts server this launcher talks to (staging and local ones too): the pictures are there.
    Promise.all([accounts.latestChangelog(), accounts.getServiceUrls()])
      .then(([latest, urls]) => {
        if (cancelled) {
          return;
        }
        if (!latest || !isNewChangelog(latest.publishedAt, seenAt, __DATE__)) {
          void markShown();
          return;
        }
        setShown({ html: changelogHtml(latest.html, urls.accountsApi), publishedAt: latest.publishedAt });
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
    <ChangelogDialogView
      open={open}
      html={shown.html}
      onClose={() => {
        setOpen(false);
        void markShown(shown.publishedAt);
      }}
    />
  );
});
