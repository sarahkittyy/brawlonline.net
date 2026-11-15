import type { SocialPlayer, SocialState } from "@social/types";
import { useCallback } from "react";

import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useToasts } from "@/lib/hooks/use_toasts";

import { useSocial } from "./use_social";

/**
 * Where the player stands with someone (docs/design/friends.md): `sent` they asked them, `incoming`
 * they were asked; `self` is the player.
 */
export type FriendRelation = "self" | "none" | "sent" | "incoming" | "friends";

export function relationOf(state: SocialState, uid: string): FriendRelation {
  if (uid === state.you) {
    return "self";
  }
  const f = state.friends;
  if (f.friends.some((p) => p.uid === uid)) {
    return "friends";
  }
  if (f.incoming.some((p) => p.uid === uid)) {
    return "incoming";
  }
  if (f.outgoing.some((p) => p.uid === uid)) {
    return "sent";
  }
  return "none";
}

export const useFriendRelation = (uid: string | undefined): FriendRelation =>
  useSocial((s) => (uid ? relationOf(s.state, uid) : "none"));

/** The uids of the player's friends (online or not). */
export const useFriendUids = (): Set<string> => {
  const friends = useSocial((s) => s.state.friends.friends);
  return new Set(friends.map((f) => f.uid));
};

/** What the friend buttons, menus and boxes do; each says how it went in a toast. */
export const useFriendActions = () => {
  const { showError, showInfo, showSuccess } = useToasts();

  /** `to`: a player, or what was typed in the code box. Resolves to whether it went through. */
  const add = useCallback(
    async (to: SocialPlayer | string): Promise<boolean> => {
      const result = await window.electron.social.sendFriendRequest(
        typeof to === "string" ? to : to.connectCode || to.uid,
      );
      if (!result.ok) {
        showError(result.message);
        return false;
      }
      if (result.status === "friends") {
        showSuccess(Messages.nowFriends(result.player.displayName));
      } else {
        showInfo(Messages.requestSent(result.player.displayName));
      }
      return true;
    },
    [showError, showInfo, showSuccess],
  );

  const accept = useCallback(
    async (player: SocialPlayer) => {
      const result = await window.electron.social.acceptFriend(player.uid);
      if (result.ok) {
        showSuccess(Messages.nowFriends(player.displayName));
      } else {
        showError(result.message);
      }
    },
    [showError, showSuccess],
  );

  const decline = useCallback(
    async (player: SocialPlayer) => {
      const result = await window.electron.social.declineFriend(player.uid);
      if (!result.ok) {
        showError(result.message);
      }
    },
    [showError],
  );

  const cancel = useCallback(
    async (player: SocialPlayer) => {
      const result = await window.electron.social.cancelFriendRequest(player.uid);
      if (!result.ok) {
        showError(result.message);
      }
    },
    [showError],
  );

  const remove = useCallback(
    async (player: SocialPlayer) => {
      const result = await window.electron.social.removeFriend(player.uid);
      if (result.ok) {
        showInfo(Messages.removedFriend(player.displayName));
      } else {
        showError(result.message);
      }
    },
    [showError, showInfo],
  );

  return { add, accept, decline, cancel, remove };
};
