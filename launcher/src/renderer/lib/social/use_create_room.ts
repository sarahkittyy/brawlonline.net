import { CREATE_ROOM } from "@dolphin/hand_off_target";
import { useCallback, useState } from "react";

import { SocialMessages } from "@/components/social/social.messages";
import { useToasts } from "@/lib/hooks/use_toasts";

import { useHandOff } from "./use_hand_off";
import { useSocial } from "./use_social";

/**
 * Creates a room in the game (Home's Create room, `brawlonline://room/create` links), with a
 * ruleset code or P+ default. Invites sent before the room shows up wait for it (SocialRoot sends
 * them); if the game doesn't take the create, the player is told whose invites were dropped.
 */
export const useCreateRoom = () => {
  const handOff = useHandOff();
  const { showWarning } = useToasts();
  const [creating, setCreating] = useState(false);

  const create = useCallback(
    async (ruleset?: string): Promise<boolean> => {
      setCreating(true);
      useSocial.getState().startPendingRoom();
      try {
        const taken = await handOff(CREATE_ROOM, ruleset);
        if (!taken) {
          const waiting = useSocial.getState().endPendingRoom();
          if (waiting.length > 0) {
            showWarning(SocialMessages.roomDidNotOpen(waiting.map((p) => p.displayName).join(", ")));
          }
        }
        return taken;
      } finally {
        setCreating(false);
      }
    },
    [handOff, showWarning],
  );

  return { create, creating };
};
