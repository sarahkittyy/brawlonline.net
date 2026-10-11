import log from "electron-log";
import { useCallback } from "react";

import { SocialMessages as Messages } from "@/components/social/social.messages";
import { usePlay } from "@/lib/dolphin/use_play";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

/**
 * Sends the game to a room (a room code) or to a Direct 1v1 with a player (their connect code),
 * the way clicking a public room does (docs/rooms-protocol.md §5): a running game gets the code,
 * else the launcher leaves it for the game and starts it like Play.
 */
export const useHandOff = () => {
  const { dolphinService } = useServices();
  const { showError, showInfo, showWarning } = useToasts();
  const play = usePlay();

  return useCallback(
    async (code: string) => {
      const direct = code.includes("#");
      try {
        const result = await dolphinService.joinRoomInGame(code);
        switch (result.outcome) {
          case "accepted":
            showInfo(direct ? Messages.goingToDirect(code) : Messages.goingToRoom());
            break;
          case "busy":
            showWarning(Messages.finishGameFirst());
            break;
          case "refused":
            showWarning(result.message || Messages.gameRefused());
            break;
          case "no-response":
            showWarning(direct ? Messages.noResponseDirect(code) : Messages.noResponseRoom(code));
            break;
          case "not-running": {
            const { id } = await dolphinService.prepareRoomLaunch(code);
            if (await play()) {
              showInfo(direct ? Messages.startingForDirect(code) : Messages.startingForRoom());
            } else {
              await dolphinService.cancelRoomLaunch(id);
            }
            break;
          }
        }
      } catch (err) {
        log.error(err);
        showError(err);
      }
    },
    [dolphinService, play, showError, showInfo, showWarning],
  );
};
