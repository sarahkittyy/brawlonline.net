import log from "electron-log";

import { useSettingsStore } from "@/lib/hooks/use_settings";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

import { volumeOf } from "./volume";

/** The sounds extracted from the player's own disc (src/brawl_assets/catalog.ts, SOUND_ROLES). */
export type SoundRole = "chat-message" | "invite";

const players = new Map<string, HTMLAudioElement>();

/**
 * Plays a notification sound at the volume from the settings, unless the bell in the header turned
 * them off or the sound isn't extracted (no disc yet, or a cache from before sounds). `preview`
 * (the settings slider) plays even with the bell off, at `volume`.
 */
export function playSound(role: SoundRole, opts: { preview?: boolean; volume?: number } = {}) {
  const settings = useSettingsStore.getState().settings;
  if (!opts.preview && !settings.notificationSounds) {
    return;
  }
  const entry = useGameAssets.getState().state?.manifest?.sounds?.[role];
  if (!entry) {
    return;
  }
  const volume = volumeOf(opts.volume ?? settings.notificationVolume);
  if (volume === 0) {
    return;
  }
  const url = assetUrl(entry.file);
  let audio = players.get(url);
  if (!audio) {
    audio = new Audio(url);
    players.set(url, audio);
  }
  audio.volume = volume;
  audio.currentTime = 0;
  audio.play().catch((err) => log.warn(`Could not play ${role}`, err));
}
