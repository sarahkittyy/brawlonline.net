import { create } from "zustand";

// How long the asked-for replays stay highlighted (their rows pulse when they appear, and the list
// scrolls to them): after this, coming back to Replays shows the list as usual.
const HIGHLIGHT_MS = 6000;

let clearTimer: ReturnType<typeof setTimeout> | undefined;

/**
 * Replays to point out in the Replays tab (the match history's Replays button): their rows pulse
 * gold, and the list scrolls to the first one. `nonce` changes with every request, so asking for
 * the same replays again pulses them again.
 */
export const useReplayHighlight = create<{
  paths: string[];
  nonce: number;
  highlight: (paths: string[]) => void;
}>((set, get) => ({
  paths: [],
  nonce: 0,
  highlight: (paths: string[]) => {
    const nonce = get().nonce + 1;
    set({ paths, nonce });
    if (clearTimer) {
      clearTimeout(clearTimer);
    }
    clearTimer = setTimeout(() => {
      if (get().nonce === nonce) {
        set({ paths: [] });
      }
    }, HIGHLIGHT_MS);
  },
}));
