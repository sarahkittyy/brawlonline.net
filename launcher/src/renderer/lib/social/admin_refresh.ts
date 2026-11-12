import { create } from "zustand";

/**
 * Bumped after an admin tool changes an account (mute, ban, rename): an admin's offline players are read again
 * at once, so a ban or a new name shows without waiting for the list's next read.
 */
export const useAdminRefresh = create<{ count: number; bump: () => void }>((set) => ({
  count: 0,
  bump: () => set((s) => ({ count: s.count + 1 })),
}));
