import log from "electron-log";
import { useCallback, useState } from "react";

import { usePlay } from "@/lib/dolphin/use_play";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";
import type { PublicRoom } from "@/services/backend/types";

import { PublicRoomsMessages as Messages } from "./public_rooms.messages";

/**
 * Clicking a room (docs/rooms-protocol.md, "Launcher and game"): a running game gets the code
 * (or the launcher says it is busy); otherwise the launcher leaves the code for the game and
 * starts it the way the Play button does.
 */
export const useJoinRoom = () => {
  const { dolphinService } = useServices();
  const { showError, showInfo, showWarning } = useToasts();
  const play = usePlay();
  const [joiningCode, setJoiningCode] = useState<string>();

  const join = useCallback(
    async (room: PublicRoom) => {
      if (!room.joinable) {
        return;
      }
      setJoiningCode(room.code);
      try {
        const result = await dolphinService.joinRoomInGame(room.code);
        switch (result.outcome) {
          case "accepted":
            showInfo(Messages.joiningInGame());
            break;
          case "busy":
            showWarning(Messages.finishGameFirst());
            break;
          case "refused":
            showWarning(result.message || Messages.gameRefused());
            break;
          case "no-response":
            showWarning(Messages.noResponse(room.code));
            break;
          case "not-running": {
            const { id } = await dolphinService.prepareRoomLaunch(room.code);
            const started = await play();
            if (started) {
              showInfo(Messages.startingForRoom());
            } else {
              await dolphinService.cancelRoomLaunch(id);
            }
            break;
          }
        }
      } catch (err) {
        log.error(err);
        showError(err);
      } finally {
        setJoiningCode(undefined);
      }
    },
    [dolphinService, play, showError, showInfo, showWarning],
  );

  return { join, joiningCode };
};
