import log from "electron-log";
import { useEffect, useState } from "react";

import { useServices } from "@/services";

/** How often the launcher asks the server again, so a restart with a new `TOURNAMENTS_URL` reaches open
 * launchers without one of their own. */
export const TOURNAMENTS_URL_REFRESH_MS = 10 * 60 * 1000;

/** The URL if it is a web page (it opens in the browser through shell.openExternal), else null. */
export function webUrl(url: string | null | undefined): string | null {
  if (!url) {
    return null;
  }
  try {
    const parsed = new URL(url);
    return parsed.protocol === "https:" || parsed.protocol === "http:" ? url : null;
  } catch {
    return null;
  }
}

/**
 * The page the header's gold trophy opens, or null for no trophy: the server's `TOURNAMENTS_URL`
 * (docs/trophy-button.md). Asked at start and every 10 minutes; an answer that fails keeps the last one
 * (none at start).
 */
export const useTournamentsUrl = (): string | null => {
  const { backendService } = useServices();
  const [url, setUrl] = useState<string | null>(null);
  useEffect(() => {
    let live = true;
    const ask = async () => {
      try {
        const answer = await backendService.fetchTournamentsUrl();
        if (live) {
          setUrl(webUrl(answer));
        }
      } catch (err) {
        log.warn("Could not ask the server for the tournaments link", err);
      }
    };
    void ask();
    const timer = window.setInterval(() => void ask(), TOURNAMENTS_URL_REFRESH_MS);
    return () => {
      live = false;
      window.clearInterval(timer);
    };
  }, [backendService]);
  return url;
};
