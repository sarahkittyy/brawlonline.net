import type { SocialPlayer, SocialRoom, SocialState } from "@social/types";
import { create } from "zustand";
import { combine } from "zustand/middleware";

/** A chat tab in the bar along the bottom of the window. */
export type ChatTab = { uid: string; open: boolean };

const emptyState: SocialState = {
  status: "off",
  online: [],
  room: null,
  ignored: [],
  conversations: {},
  invites: [],
};

/**
 * The social state from the main process (docs/social-protocol.md) and the chat tabs, which only
 * the renderer has: tabs open from the bottom left to the right, in the order they were opened.
 */
export const useSocial = create(
  combine({ state: emptyState, tabs: [] as ChatTab[] }, (set, get) => ({
    setState: (state: SocialState) => set({ state }),
    /** Opens (and expands, unless `expand` is false) the player's tab, adding it if needed. */
    openTab: (uid: string, expand = true) => {
      const tabs = get().tabs;
      const existing = tabs.find((t) => t.uid === uid);
      if (existing) {
        if (expand && !existing.open) {
          set({ tabs: tabs.map((t) => (t.uid === uid ? { ...t, open: true } : t)) });
        }
        return;
      }
      set({ tabs: [...tabs, { uid, open: expand }] });
    },
    toggleTab: (uid: string) => set({ tabs: get().tabs.map((t) => (t.uid === uid ? { ...t, open: !t.open } : t)) }),
    closeTab: (uid: string) => set({ tabs: get().tabs.filter((t) => t.uid !== uid) }),
  })),
);

/** Why "Invite to game" is greyed out, or undefined when it can be used. */
export function inviteBlocked(room: SocialRoom | null): "full" | undefined {
  return room && !room.joinable ? "full" : undefined;
}

/** A player by uid: the online list first, then the conversations (players who left). */
export function findPlayer(state: SocialState, uid: string): SocialPlayer | undefined {
  return state.online.find((p) => p.uid === uid) ?? state.conversations[uid]?.player;
}
