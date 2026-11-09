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
 * Whether this client has ever shown the player their own profile. Until it has, the header's name glows gold and so
 * does the user menu's View Profile (user_menu.tsx); once any account opens its own profile, from anywhere, neither
 * glows on this client again.
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
