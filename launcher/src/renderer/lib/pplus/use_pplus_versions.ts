// The launcher's Project+ release and the older ones kept for replays (docs/design/pplus-versions.md),
// for the replay rows' version tags, the stats page and Settings > Replays.
import type { PPlusVersionInfo } from "@dolphin/types";
import { create } from "zustand";

type PPlusVersionsStore = {
  current: string | null;
  states: PPlusVersionInfo[];
  loaded: boolean;
  setVersions: (current: string, states: PPlusVersionInfo[]) => void;
};

export const usePPlusVersions = create<PPlusVersionsStore>((set) => ({
  current: null,
  states: [],
  loaded: false,
  setVersions: (current, states) => set({ current, states, loaded: true }),
}));

let loading: Promise<void> | null = null;

/** Reads them again from main (after a download, a removal, or once at start). */
export function refreshPPlusVersions(): Promise<void> {
  if (!loading) {
    loading = window.electron.dolphin
      .getPPlusVersions()
      .then(({ current, states }) => usePPlusVersions.getState().setVersions(current, states))
      .catch(() => undefined)
      .finally(() => {
        loading = null;
      });
  }
  return loading;
}

/** The state of `release` when it is not the launcher's own (undefined: it is, or unknown). */
export function useOlderRelease(release: string | undefined | null): PPlusVersionInfo | "unknown" | undefined {
  const current = usePPlusVersions((s) => s.current);
  const state = usePPlusVersions((s) => s.states.find((x) => x.release === release));
  if (!release || !current || release === current) {
    return undefined;
  }
  return state ?? "unknown";
}
