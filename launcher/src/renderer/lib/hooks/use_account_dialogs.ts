import { create } from "zustand";
import { combine } from "zustand/middleware";

/** The account dialogs the player's own profile opens; the header's user menu (user_menu.tsx) shows them. */
export type AccountDialog = "switch" | "logout" | "activate" | "verify";

/**
 * Which account dialog is open. The header's name opens the player's own profile; its footer (profile_dialog.tsx)
 * has Switch account and Log out, and Activate online play / Verify email until they're done.
 */
export const useAccountDialogs = create(
  combine({ open: null as AccountDialog | null }, (set) => ({
    show: (dialog: AccountDialog) => set({ open: dialog }),
    close: () => set({ open: null }),
  })),
);
