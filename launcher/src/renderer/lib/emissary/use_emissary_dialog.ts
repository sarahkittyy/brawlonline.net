// EmissaryRecomp's popup (emissary_dialog.tsx): open or not, and what its Play goes on with. Play opens it (use_play.ts).
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { refreshEmissary } from "./use_emissary";

/**
 * What the popup's Play does once EmissaryRecomp is set up: what was asked for when the popup opened, so a room,
 * an invite, Create room or a queue goes on to it instead of a plain Play. Resolves to whether the game started.
 */
export type EmissaryResume = () => Promise<boolean>;

export const useEmissaryDialog = create(
  combine({ open: false, resume: null as EmissaryResume | null }, (set) => ({
    openDialog: (resume?: EmissaryResume) => {
      set({ open: true, resume: resume ?? null });
      void refreshEmissary(true);
    },
    closeDialog: () => set({ open: false, resume: null }),
  })),
);
