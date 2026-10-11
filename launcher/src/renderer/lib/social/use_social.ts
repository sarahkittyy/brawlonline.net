import type { SocialPlayer, SocialRoom, SocialState } from "@social/types";
import { create } from "zustand";
import { combine } from "zustand/middleware";

/**
 * A chat tab in the bar along the bottom of the window. `z` orders the open windows where they
 * overlap: the one raised last (opened or clicked) is drawn over the others.
 */
export type ChatTab = { uid: string; open: boolean; z: number };

/** The open window drawn over the others, if any. */
export function frontTab(tabs: ChatTab[]): string | null {
  let front: ChatTab | null = null;
  for (const t of tabs) {
    if (t.open && (!front || t.z > front.z)) {
      front = t;
    }
  }
  return front?.uid ?? null;
}

const topZ = (tabs: ChatTab[]) => tabs.reduce((z, t) => Math.max(z, t.z), 0) + 1;

/** Opens (on top) or minimizes a tab's window. */
const toggled = (tabs: ChatTab[], uid: string) =>
  tabs.map((t) => (t.uid === uid ? { ...t, open: !t.open, z: t.open ? t.z : topZ(tabs) } : t));

const raised = (tabs: ChatTab[], uid: string) =>
  frontTab(tabs) === uid ? tabs : tabs.map((t) => (t.uid === uid ? { ...t, z: topZ(tabs) } : t));

/**
 * Home's "Create room" was clicked and the room isn't there yet: invites sent meanwhile wait for it
 * (they go to the new room, not to a Direct 1v1). `fromCode` is the room the player was in when
 * clicking (the game leaves it), so only a different room counts as the new one.
 */
export type PendingRoom = { since: number; fromCode: string | null; invites: SocialPlayer[] };

/** How long a room the player asked for may take to show up (the game may have to start first). */
export const PENDING_ROOM_MS = 120_000;

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
  combine({ state: emptyState, tabs: [] as ChatTab[], pendingRoom: null as PendingRoom | null }, (set, get) => ({
    setState: (state: SocialState) => set({ state }),
    startPendingRoom: () =>
      set({ pendingRoom: { since: Date.now(), fromCode: get().state.room?.code ?? null, invites: [] } }),
    /** Ends the wait and hands back the invites that waited for the room. */
    endPendingRoom: (): SocialPlayer[] => {
      const invites = get().pendingRoom?.invites ?? [];
      set({ pendingRoom: null });
      return invites;
    },
    queueInvite: (player: SocialPlayer) => {
      const pending = get().pendingRoom;
      if (pending && !pending.invites.some((p) => p.uid === player.uid)) {
        set({ pendingRoom: { ...pending, invites: [...pending.invites, player] } });
      }
    },
    /** Opens (and expands, unless `expand` is false) the player's tab, adding it if needed. */
    openTab: (uid: string, expand = true) => {
      const tabs = get().tabs;
      const existing = tabs.find((t) => t.uid === uid);
      if (existing) {
        if (expand && (!existing.open || frontTab(tabs) !== uid)) {
          set({ tabs: tabs.map((t) => (t.uid === uid ? { ...t, open: true, z: topZ(tabs) } : t)) });
        }
        return;
      }
      set({ tabs: [...tabs, { uid, open: expand, z: topZ(tabs) }] });
    },
    toggleTab: (uid: string) => set({ tabs: toggled(get().tabs, uid) }),
    /** Draws an open window over the others. */
    raiseTab: (uid: string) => set({ tabs: raised(get().tabs, uid) }),
    /** A tab's header: brings its window to the front if another covers it, else opens or minimizes it. */
    clickTab: (uid: string) => {
      const tabs = get().tabs;
      const covered = tabs.some((t) => t.uid === uid && t.open) && frontTab(tabs) !== uid;
      set({ tabs: covered ? raised(tabs, uid) : toggled(tabs, uid) });
    },
    closeTab: (uid: string) => set({ tabs: get().tabs.filter((t) => t.uid !== uid) }),
  })),
);

/** The new room, once the room the player asked for has shown up. */
export function arrivedRoom(pending: PendingRoom | null, room: SocialRoom | null): SocialRoom | null {
  return pending && room && room.code !== pending.fromCode ? room : null;
}

/** Whether an invite now waits for a room the player asked for. */
export function waitsForRoom(pending: PendingRoom | null, room: SocialRoom | null): boolean {
  return pending != null && arrivedRoom(pending, room) == null;
}

/** Why "Invite to game" is greyed out, or undefined when it can be used. */
export function inviteBlocked(room: SocialRoom | null): "full" | undefined {
  return room && !room.joinable ? "full" : undefined;
}

/** A player by uid: the online list first, then the conversations (players who left). */
export function findPlayer(state: SocialState, uid: string): SocialPlayer | undefined {
  return state.online.find((p) => p.uid === uid) ?? state.conversations[uid]?.player;
}
