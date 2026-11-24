// EmissaryRecomp's popup (emissary_dialog.tsx): open or not. Play opens it (use_play.ts).
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { refreshEmissary } from "./use_emissary";

export const useEmissaryDialog = create(
  combine({ open: false }, (set) => ({
    openDialog: () => {
      set({ open: true });
      void refreshEmissary(true);
    },
    closeDialog: () => set({ open: false }),
  })),
);
