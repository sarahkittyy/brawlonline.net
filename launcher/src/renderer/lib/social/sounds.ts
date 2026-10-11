import log from "electron-log";

import { useSettingsStore } from "@/lib/hooks/use_settings";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

/** The sounds extracted from the player's own disc (src/brawl_assets/catalog.ts, SOUND_ROLES). */
export type SoundRole = "chat-message" | "invite";

const VOLUME = 0.6;
const players = new Map<string, HTMLAudioElement>();

/**
 * Plays a notification sound, unless the bell in the header turned them off or the sound isn't
 * extracted (no disc yet, or a cache from before sounds).
 */
export function playSound(role: SoundRole) {
  if (!useSettingsStore.getState().settings.notificationSounds) {
    return;
  }
  const entry = useGameAssets.getState().state?.manifest?.sounds?.[role];
  if (!entry) {
    return;
  }
  const url = assetUrl(entry.file);
  let audio = players.get(url);
  if (!audio) {
    audio = new Audio(url);
    audio.volume = VOLUME;
    players.set(url, audio);
  }
  audio.currentTime = 0;
  audio.play().catch((err) => log.warn(`Could not play ${role}`, err));
}
