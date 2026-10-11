// Picks GTK's input method on SteamOS and in Steam's Gaming Mode (gamescope).
//
// There the GTK input module the session sets up sends each key to Chromium twice, so every letter
// typed in chat came out doubled (staging feedback 2026-10-11, SteamOS; the player fixed it with
// `GTK_IM_MODULE=simple %command%`). GTK's built-in "simple" module types plain keys and dead keys
// only. Chromium creates its GTK input context when a text field first takes focus and GTK reads
// GTK_IM_MODULE then, so setting it at startup is early enough. PPO_GTK_IM_MODULE picks another
// module (e.g. `PPO_GTK_IM_MODULE=ibus %command%` for an IME), or "keep" leaves GTK_IM_MODULE as is.
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type InputMethodChoice =
  /** Not SteamOS or gamescope, or PPO_GTK_IM_MODULE=keep: GTK_IM_MODULE is left alone. */
  | { kind: "unchanged" }
  /** Set GTK_IM_MODULE to `module` (it was `previous`). */
  | { kind: "set"; module: string; previous: string | undefined };

/** Whether /etc/os-release names SteamOS (`ID=steamos`, unquoted or quoted). */
export function isSteamOs(osRelease: string | null): boolean {
  return osRelease !== null && /^ID=["']?steamos["']?\s*$/m.test(osRelease);
}

/** Whether this runs in a gamescope session (Steam's Gaming Mode on SteamOS, Bazzite, ChimeraOS). */
export function isGamescope(env: NodeJS.ProcessEnv): boolean {
  return env.XDG_CURRENT_DESKTOP?.toLowerCase() === "gamescope" || Boolean(env.GAMESCOPE_WAYLAND_DISPLAY);
}

export function chooseGtkInputMethod(
  platform: NodeJS.Platform,
  env: NodeJS.ProcessEnv,
  osRelease: string | null,
): InputMethodChoice {
  if (platform !== "linux" || !(isSteamOs(osRelease) || isGamescope(env))) {
    return { kind: "unchanged" };
  }
  const wanted = env.PPO_GTK_IM_MODULE || "simple";
  if (wanted === "keep" || wanted === env.GTK_IM_MODULE) {
    return { kind: "unchanged" };
  }
  return { kind: "set", module: wanted, previous: env.GTK_IM_MODULE };
}
