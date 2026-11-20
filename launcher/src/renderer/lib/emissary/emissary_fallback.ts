import type { EmissaryFallback } from "@dolphin/emissary/types";

import { AddonsSettingsMessages as Messages } from "@/pages/settings/addons_settings/addons_settings.messages";

/** A sentence: ends with its own full stop. */
const sentence = (text: string) => (/[.!?]$/.test(text.trim()) ? text.trim() : `${text.trim()}.`);

/**
 * What the player is told when Play used Dolphin although Settings > Game says EmissaryRecomp
 * (docs/design/brawl-recompiled.md §4.3: never silently).
 */
export function emissaryFallbackText(fallback: EmissaryFallback): string {
  switch (fallback.reason) {
    case "noOnline":
      return Messages.noOnline();
    case "status":
      return Messages.playUsesDolphin(Messages.statusText(fallback.status));
    case "updateFailed":
      return Messages.playUsesDolphin(sentence(fallback.message));
  }
}
