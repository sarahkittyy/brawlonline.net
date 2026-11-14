import type React from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { useAccount } from "@/lib/hooks/use_account";

/**
 * A name the right-click menu opens on. Some lists only have the name (rooms, live games): the menu takes the
 * connect code from the player's profile (components/social/player_menu.tsx, PlayerMenuHost).
 */
export type PlayerMenuName = { uid: string; displayName: string; connectCode?: string; country?: string | null };

export type PlayerMenuRequest = { player: PlayerMenuName; x: number; y: number; showMessage: boolean };

/**
 * The one player menu every hover-card name opens (the online list's own menu, from anywhere): the rooms' names,
 * the live games, the leaderboard, the match history, a chat window's title. SocialRoot shows it.
 */
export const usePlayerMenu = create(
  combine({ request: null as PlayerMenuRequest | null }, (set) => ({
    open: (request: PlayerMenuRequest) => set({ request }),
    close: () => set({ request: null }),
  })),
);

/**
 * A name's onContextMenu: opens the player menu on `player`. Undefined (the right click does nothing) when the
 * player isn't known, it's the logged-in player's own name, or no one is logged in to chat (no menu then).
 */
export function usePlayerMenuOpener(
  player: { uid?: string; displayName: string; connectCode?: string; country?: string | null } | undefined,
  { showMessage = true, enabled = true }: { showMessage?: boolean; enabled?: boolean } = {},
): ((e: React.MouseEvent) => void) | undefined {
  const me = useAccount((s) => (s.user != null && s.userData?.playKey != null ? s.user.uid : undefined));
  const open = usePlayerMenu((s) => s.open);
  const uid = player?.uid;
  if (!enabled || !player || !uid || !me || uid === me || player.displayName === "") {
    return undefined;
  }
  return (e: React.MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    open({
      player: { uid, displayName: player.displayName, connectCode: player.connectCode, country: player.country },
      x: e.clientX,
      y: e.clientY,
      showMessage,
    });
  };
}
