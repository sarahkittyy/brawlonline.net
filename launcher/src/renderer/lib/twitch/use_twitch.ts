import type { TwitchLink, TwitchStream } from "@accounts/types";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useMemo } from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { useAccount } from "@/lib/hooks/use_account";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

/**
 * Twitch in the launcher (docs/design/twitch-live.md): the live streams of players playing Brawl Online, and the
 * logged-in player's own Twitch connection.
 */

/** How often the streams are read (the server asks Twitch once a minute). */
export const TWITCH_POLL_MS = 60_000;
/** While a connect waits for Twitch's sign-in in the browser, the connection is read this often. */
const CONNECT_POLL_MS = 3_000;
/** A connect stops waiting after this long (the player closed the browser tab). */
const CONNECT_WAIT_MS = 5 * 60_000;

const NO_STREAMS: TwitchStream[] = [];

/** The live streams, most viewers first (none while logged out). Shared by every place that shows them. */
export const useTwitchStreams = (): TwitchStream[] => {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const { data } = useQuery({
    queryKey: ["twitchStreams", uid],
    queryFn: () => backendService.fetchTwitchStreams(),
    enabled: uid != null,
    refetchInterval: TWITCH_POLL_MS,
    staleTime: TWITCH_POLL_MS / 2,
    retry: false,
  });
  return data ?? NO_STREAMS;
};

/** A player's live stream, or null when they aren't live. */
export const useTwitchStream = (uid: string | undefined): TwitchStream | null => {
  const streams = useTwitchStreams();
  return useMemo(() => (uid ? streams.find((s) => s.uid === uid) ?? null : null), [streams, uid]);
};

/** The twitch.tv page of a login, in the browser. */
export const openTwitch = (login: string) => {
  window.electron.shell.openExternal(`https://www.twitch.tv/${encodeURIComponent(login)}`).catch(() => undefined);
};

/** When the player last opened Twitch's sign-in (null: not waiting for it). */
const useConnectWait = create(
  combine({ since: null as number | null }, (set) => ({
    start: () => set({ since: Date.now() }),
    stop: () => set({ since: null }),
  })),
);

/**
 * The logged-in player's Twitch connection. While a connect waits for Twitch's sign-in it is read every few seconds;
 * once connected, the profile and the streams are read again.
 */
export const useTwitchLink = (): TwitchLink | undefined => {
  const { backendService } = useServices();
  const queryClient = useQueryClient();
  const uid = useAccount((s) => s.user?.uid);
  const since = useConnectWait((s) => s.since);
  const stop = useConnectWait((s) => s.stop);
  const { data } = useQuery({
    queryKey: ["twitchLink", uid],
    queryFn: () => backendService.fetchTwitchLink(),
    enabled: uid != null,
    staleTime: 5 * 60_000,
    refetchOnWindowFocus: true,
    refetchInterval: since != null ? CONNECT_POLL_MS : false,
    retry: false,
  });
  const linked = data?.linked?.login ?? null;
  useEffect(() => {
    if (since == null) {
      return;
    }
    if (linked) {
      stop();
      void queryClient.invalidateQueries({ queryKey: ["profile", uid] });
      void queryClient.invalidateQueries({ queryKey: ["twitchStreams"] });
      return;
    }
    const left = since + CONNECT_WAIT_MS - Date.now();
    const timer = setTimeout(stop, Math.max(0, left));
    return () => clearTimeout(timer);
  }, [since, linked, stop, queryClient, uid]);
  return data;
};

/** Connect (opens Twitch's sign-in in the browser) and Disconnect, and whether a connect is waiting. */
export const useTwitchActions = () => {
  const { backendService } = useServices();
  const queryClient = useQueryClient();
  const { showError } = useToasts();
  const uid = useAccount((s) => s.user?.uid);
  const waiting = useConnectWait((s) => s.since != null);
  const start = useConnectWait((s) => s.start);
  return {
    waiting,
    connect: async () => {
      try {
        const url = await backendService.twitchConnectUrl();
        start();
        await window.electron.shell.openExternal(url);
      } catch (err) {
        showError(err);
      }
    },
    disconnect: async () => {
      try {
        await backendService.disconnectTwitch();
      } catch (err) {
        showError(err);
      } finally {
        await queryClient.invalidateQueries({ queryKey: ["twitchLink", uid] });
        await queryClient.invalidateQueries({ queryKey: ["twitchStreams"] });
      }
    },
  };
};

const HINT_DONE_KEY = "TWITCH_CONNECT_HINT_DONE";

const hintDoneAtStart = (() => {
  try {
    return localStorage.getItem(HINT_DONE_KEY) === "true";
  } catch {
    return false;
  }
})();

/** Whether the "connect your Twitch" hint has been followed once on this client (its pencil clicked). */
export const useTwitchHintDone = create(
  combine({ done: hintDoneAtStart }, (set) => ({
    markDone: () => {
      try {
        localStorage.setItem(HINT_DONE_KEY, "true");
      } catch {
        // Ignore storage errors: it glows again next launch.
      }
      set({ done: true });
    },
  })),
);

/**
 * A player who typed a Twitch handle into their profile but never connected the account gets the profile glow back
 * (Sarah's board): the sidebar's card, then their profile's pencil, then Connect account. Clicking the pencil ends it on
 * this client, connected or not.
 */
export const useTwitchHint = (): boolean => {
  const link = useTwitchLink();
  const done = useTwitchHintDone((s) => s.done);
  return !done && link != null && link.available && !!link.handle && link.linked == null;
};

/** The live streams by player uid. */
export const useTwitchStreamMap = (): Map<string, TwitchStream> => {
  const streams = useTwitchStreams();
  return useMemo(() => new Map(streams.map((s) => [s.uid, s])), [streams]);
};
