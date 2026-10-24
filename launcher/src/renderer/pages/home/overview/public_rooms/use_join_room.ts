import { useCallback, useState } from "react";

import { useHandOff } from "@/lib/social/use_hand_off";

/**
 * Joining a room by its code: a click on a public room, or a code typed in Home's room code box
 * (docs/rooms-protocol.md, "Launcher and game"): a running game gets the code (or the launcher says
 * it is busy); otherwise the launcher leaves the code for the game and starts it the way the Play
 * button does. Resolves to whether the game took it (or is starting with it).
 */
export const useJoinRoom = () => {
  const handOff = useHandOff();
  const [joiningCode, setJoiningCode] = useState<string>();

  const join = useCallback(
    async (code: string): Promise<boolean> => {
      setJoiningCode(code);
      try {
        return await handOff(code);
      } finally {
        setJoiningCode(undefined);
      }
    },
    [handOff],
  );

  return { join, joiningCode };
};
