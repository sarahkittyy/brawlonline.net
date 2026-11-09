import { create } from "zustand";
import { combine } from "zustand/middleware";

/** Whose profile to show: the uid, and the name and code the opener already had (shown while it loads). */
export type ProfileTarget = { uid: string; displayName?: string; connectCode?: string };

/**
 * The profile dialog (components/profile/profile_dialog.tsx), opened from anywhere a name is: the user menu's
 * View Profile, a player's right-click menu, a chat window's title, the match history and the leaderboard.
 */
export const useProfileView = create(
  combine({ target: null as ProfileTarget | null }, (set) => ({
    open: (target: ProfileTarget) => set({ target }),
    close: () => set({ target: null }),
  })),
);
