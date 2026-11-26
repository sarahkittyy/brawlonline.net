import { CREATE_ROOM } from "@dolphin/hand_off_target";
import { useCallback, useRef, useState } from "react";

import { SocialMessages } from "@/components/social/social.messages";
import { useToasts } from "@/lib/hooks/use_toasts";

import { useHandOff } from "./use_hand_off";
import { useSocial } from "./use_social";

/**
 * Creates a room in the game (Home's Create room, `brawlonline://room/create` links), with a
 * ruleset code or P+ default, public unless `isPrivate` (the website's CREATE PRIVATE ROOM). Invites sent before the room shows up wait for it (SocialRoot sends
 * them); if the game doesn't take the create, the player is told whose invites were dropped. When EmissaryRecomp's
 * popup opens first, its Play creates the room again this way.
 */
export const useCreateRoom = () => {
  const handOff = useHandOff();
  const { showWarning } = useToasts();
  const [creating, setCreating] = useState(false);
  const createRef = useRef<(ruleset?: string, isPrivate?: boolean) => Promise<boolean>>(async () => false);

  const create = useCallback(
    async (ruleset?: string, isPrivate?: boolean): Promise<boolean> => {
      setCreating(true);
      useSocial.getState().startPendingRoom();
      try {
        const taken = await handOff(CREATE_ROOM, ruleset, isPrivate, () => createRef.current(ruleset, isPrivate));
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
  createRef.current = create;

  return { create, creating };
};
