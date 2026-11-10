import { create } from "zustand";
import { combine } from "zustand/middleware";

const OWN_PROFILE_OPENED_KEY = "OWN_PROFILE_OPENED";

const startingValue = (() => {
  try {
    return localStorage.getItem(OWN_PROFILE_OPENED_KEY) === "true";
  } catch {
    return false;
  }
})();

/**
 * Whether this client's player has found their profile's edit pencil. Until then the header's name glows gold
 * (user_menu.tsx: clicking it opens their own profile) and in their own profile the pencil does (profile_dialog.tsx);
 * once any account clicks that pencil, even without changing anything, neither glows on this client again.
 *
 * The key is the one the older hint used ("opened their own profile once"), so whoever has already seen theirs that
 * way doesn't get the glow back.
 */
export const useOwnProfileHint = create(
  combine({ opened: startingValue }, (set) => ({
    markOpened: () => {
      try {
        localStorage.setItem(OWN_PROFILE_OPENED_KEY, "true");
      } catch {
        // Ignore storage errors: it glows again next launch.
      }
      set({ opened: true });
    },
  })),
);
