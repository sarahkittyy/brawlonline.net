import type { SocialPlayer } from "@social/types";
import { useCallback } from "react";

import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useToasts } from "@/lib/hooks/use_toasts";

import { useSocial, waitsForRoom } from "./use_social";

/** What a player's name, menu and invite button do. */
export const useSocialActions = () => {
  const { showError, showInfo } = useToasts();
  const openTab = useSocial((s) => s.openTab);

  const invite = useCallback(
    async (player: SocialPlayer) => {
      const { pendingRoom, state, queueInvite } = useSocial.getState();
      if (waitsForRoom(pendingRoom, state.room)) {
        queueInvite(player);
        showInfo(Messages.inviteWaitsForRoom(player.displayName));
        return;
      }
      const result = await window.electron.social.invite(player.uid);
      if (result.ok) {
        showInfo(
          result.kind === "room"
            ? Messages.invitedRoom(player.displayName)
            : Messages.invitedDirect(player.displayName),
        );
      } else {
        showError(result.message);
      }
    },
    [showError, showInfo],
  );

  const setIgnored = useCallback(
    async (player: SocialPlayer, ignored: boolean) => {
      try {
        await window.electron.social.setIgnored(player.uid, ignored);
      } catch (err) {
        showError(err);
      }
    },
    [showError],
  );

  const message = useCallback((player: SocialPlayer) => openTab(player.uid, true), [openTab]);

  return { invite, setIgnored, message };
};
