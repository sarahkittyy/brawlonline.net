/**
 * The list a highlight was played from (docs/design/highlights.md §3, the user 2026-10-24): the
 * playback Dolphin's "3/40  << < > >>" and stars come from it, and its row shows as playing.
 *
 * The order the buttons go in: the list's replays in the order they first come in it, each one's
 * highlights by where they start (the same as the list sorted Newest first). "<" / ">" go to the
 * highlight before / after the frame shown, in this replay first (Dolphin does those itself, from
 * the stars), then the replay before / after it; "<<" / ">>" to the replay before / after, its
 * first highlight. The list is the one played from last (a stats page's, or the Highlights tab's
 * section): another section opened there doesn't change what plays, but the buttons then go by
 * it, and Dolphin's count and stars follow it at once.
 *
 * The store lives as long as the launcher's window, so the buttons work with the Highlights page
 * closed: `useHighlightNavigation` (highlight_playlist.ts, mounted once in app/create.tsx) answers
 * Dolphin.
 */
import type { DolphinService, ReplayPlaybackStatus } from "@dolphin/types";
import type { HighlightListView, HighlightPlayItem } from "@replays/highlights/types";
import { create } from "zustand";

export type PlaylistEntry = HighlightPlayItem & { key: string };
export type NavAction = NonNullable<ReplayPlaybackStatus["navRequest"]>["action"];

/** A highlight `<` this far into plays again from its start; nearer its start, the one before (as Dolphin's). */
const PREV_RESTARTS_AFTER = 60;
/** The list's changes go to Dolphin at most this often (the Highlights tab fills replay by replay). */
const UPDATE_EVERY_MS = 400;

const sameClip = (a: HighlightPlayItem, b: HighlightPlayItem) =>
  a.path === b.path && a.startFrame === b.startFrame && a.endFrame === b.endFrame;

/** The buttons' order: the list's replays as they first come in it, each one's highlights by their start. */
export function navOrder(entries: readonly PlaylistEntry[]): PlaylistEntry[][] {
  const groups = new Map<string, PlaylistEntry[]>();
  for (const e of entries) {
    const g = groups.get(e.path);
    if (g) {
      g.push(e);
    } else {
      groups.set(e.path, [e]);
    }
  }
  return [...groups.values()].map((g) => [...g].sort((a, b) => a.startFrame - b.startFrame || a.endFrame - b.endFrame));
}

/** What Dolphin shows of the list while `current` plays: this replay's stars and the count. */
export function listView(entries: readonly PlaylistEntry[], current: HighlightPlayItem): HighlightListView {
  const groups = navOrder(entries);
  const flat = groups.flat();
  const g = groups.findIndex((group) => group[0].path === current.path);
  const position = flat.findIndex((e) => sameClip(e, current)) + 1;
  const marks =
    g >= 0
      ? groups[g].map((e) => ({ startFrame: e.startFrame, endFrame: e.endFrame, position: flat.indexOf(e) + 1 }))
      : [];
  return {
    marks,
    nav: {
      position,
      total: flat.length,
      // (A replay not in the list: every one of it is before and after it.)
      hasPrevReplay: g >= 0 ? g > 0 : groups.length > 0,
      hasNextReplay: g >= 0 ? g < groups.length - 1 : groups.length > 0,
    },
  };
}

/** Where a list button goes from frame `frame` of the replay `path`, or null (nothing there). */
export function navTarget(
  entries: readonly PlaylistEntry[],
  at: { path: string; frame: number },
  action: NavAction,
): PlaylistEntry | null {
  const groups = navOrder(entries);
  if (!groups.length) {
    return null;
  }
  const g = groups.findIndex((group) => group[0].path === at.path);
  if (g < 0) {
    // Not one of the list's replays (another section was opened): its first or last.
    switch (action) {
      case "nextClip":
      case "nextReplay":
        return groups[0][0];
      case "prevClip":
        return groups[groups.length - 1].at(-1) ?? null;
      case "prevReplay":
        return groups[groups.length - 1][0];
    }
  }
  const group = groups[g];
  switch (action) {
    case "nextClip":
      return group.find((e) => e.startFrame > at.frame) ?? groups[g + 1]?.[0] ?? null;
    case "prevClip":
      return (
        [...group].reverse().find((e) => e.startFrame < at.frame - PREV_RESTARTS_AFTER) ?? groups[g - 1]?.at(-1) ?? null
      );
    case "nextReplay":
      return groups[g + 1]?.[0] ?? null;
    case "prevReplay":
      return groups[g - 1]?.[0] ?? null;
  }
}

type PlaylistState = {
  /** Which list (HighlightRows' `sourceId`) it is, and its rows in the list's order. */
  source: string | null;
  entries: PlaylistEntry[];
  /** What plays (one of the list's, or a star Dolphin played that isn't), null once Dolphin closed. */
  current: PlaylistEntry | null;
  /** The request's commandId as Dolphin has it (Play anyway asks again as `<id>+anyway`), and the last nav / pick answered. */
  commandId: string | null;
  navSeq: number;
  pickSeq: number;
};

export const usePlaylistStore = create<PlaylistState>(() => ({
  source: null,
  entries: [],
  current: null,
  commandId: null,
  navSeq: 0,
  pickSeq: 0,
}));

export const newCommandId = () => `hl-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;

let updateTimer: ReturnType<typeof setTimeout> | null = null;

/** The list `source` shows `entries` now: if it is the one played from, Dolphin's stars and count follow (throttled). */
export function setPlaylistEntries(source: string, entries: PlaylistEntry[], dolphinService: DolphinService) {
  const state = usePlaylistStore.getState();
  if (state.source !== source) {
    return;
  }
  usePlaylistStore.setState({ entries });
  if (!state.commandId || !state.current || updateTimer) {
    return;
  }
  updateTimer = setTimeout(() => {
    updateTimer = null;
    const { entries: now, current, commandId } = usePlaylistStore.getState();
    if (current && commandId) {
      const item = { path: current.path, startFrame: current.startFrame, endFrame: current.endFrame };
      void dolphinService.updateHighlightList(item, commandId, listView(now, item)).catch(() => undefined);
    }
  }, UPDATE_EVERY_MS);
}

/** What `useHighlightNavigation` does with a status of the request playing (exposed for tests). */
export function followStatus(
  status: ReplayPlaybackStatus,
  play: (source: string, entries: PlaylistEntry[], entry: PlaylistEntry) => void,
) {
  const state = usePlaylistStore.getState();
  if (!state.commandId || !status.commandId?.startsWith(state.commandId.split("+")[0])) {
    return;
  }
  if (status.commandId !== state.commandId) {
    usePlaylistStore.setState({ commandId: status.commandId });
  }
  const path = status.replay || state.current?.path;
  const pick = status.highlightPick;
  if (pick && pick.seq > state.pickSeq && path) {
    // Dolphin played a star (or < >) of this replay itself: that row is the one playing now.
    const clip = { path, startFrame: pick.startFrame, endFrame: pick.endFrame };
    const entry = state.entries.find((e) => sameClip(e, clip)) ?? { ...clip, key: "" };
    usePlaylistStore.setState({ pickSeq: pick.seq, current: entry });
  }
  const nav = status.navRequest;
  if (nav && nav.seq > state.navSeq && path && state.source) {
    usePlaylistStore.setState({ navSeq: nav.seq });
    const target = navTarget(state.entries, { path, frame: nav.frame }, nav.action);
    if (target) {
      play(state.source, state.entries, target);
    }
  }
}
