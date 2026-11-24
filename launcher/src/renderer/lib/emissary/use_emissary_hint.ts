import { create } from "zustand";
import { combine } from "zustand/middleware";

import { useEmissary } from "./use_emissary";

const ADDONS_SEEN_KEY = "EMISSARY_ADDONS_SEEN";

const startingValue = (() => {
  try {
    return localStorage.getItem(ADDONS_SEEN_KEY) === "true";
  } catch {
    return false;
  }
})();

/**
 * Whether this client has been to Settings > Add-ons since EmissaryRecomp came to it. Until then the way there glows
 * gold, one step at a time (Sarah's board, 2026-11-24: "the settings glowing, and then add-ons glowing, and then the
 * recomp card glowing, and this glow happening once ever for new installs and fresh updates and then never again once
 * you land on the add-ons page"): the sidebar's Settings button, then Settings' Add-ons entry, then the card, which
 * glows on that first visit only (addons_settings.tsx). The same gold as the profile hint (use_own_profile_hint.ts).
 */
export const useEmissaryHint = create(
  combine({ seen: startingValue }, (set) => ({
    markSeen: () => {
      try {
        localStorage.setItem(ADDONS_SEEN_KEY, "true");
      } catch {
        // Ignore storage errors: it glows again next launch.
      }
      set({ seen: true });
    },
  })),
);

/** The way to Add-ons glows: not seen yet, and EmissaryRecomp has releases for this computer (not macOS for now). */
export const useEmissaryHintGlows = () => {
  const seen = useEmissaryHint((s) => s.seen);
  const supported = useEmissary((s) => s.state?.supported === true && s.state.status !== "unavailable");
  return !seen && supported;
};
