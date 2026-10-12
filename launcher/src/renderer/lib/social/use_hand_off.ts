import { CREATE_ROOM, isQueueTarget, QUEUE_RANKED } from "@dolphin/hand_off_target";
import log from "electron-log";
import { useCallback } from "react";

import { SocialMessages as Messages } from "@/components/social/social.messages";
import { usePlay } from "@/lib/dolphin/use_play";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

/**
 * Sends the game to a room (a room code), to a Direct 1v1 with a player (their connect code) or
 * to a new room of its own (`CREATE_ROOM`) or to the Unranked or Ranked character select (`QUEUE_UNRANKED`,
 * `QUEUE_RANKED`), the way clicking a public room does (docs/rooms-protocol.md §5): a running game gets the code,
 * else the launcher leaves it for the game and starts it like Play. A create can carry a ruleset code
 * (docs/rulesets-protocol.md §4). Resolves to whether the game took it (or is starting with it).
 */
export const useHandOff = () => {
  const { dolphinService } = useServices();
  const { showError, showInfo, showWarning } = useToasts();
  const play = usePlay();

  return useCallback(
    async (code: string, ruleset?: string): Promise<boolean> => {
      const kind =
        code === CREATE_ROOM ? "create" : isQueueTarget(code) ? "queue" : code.includes("#") ? "direct" : "room";
      const queue = code === QUEUE_RANKED ? "Ranked" : "Unranked";
      const text = {
        accepted: {
          queue: Messages.goingToQueue(queue),
          create: Messages.creatingRoom(),
          direct: Messages.goingToDirect(code),
          room: Messages.goingToRoom(),
        },
        noResponse: {
          queue: Messages.noResponseQueue(queue),
          create: Messages.noResponseCreate(),
          direct: Messages.noResponseDirect(code),
          room: Messages.noResponseRoom(code),
        },
        starting: {
          queue: Messages.startingForQueue(queue),
          create: Messages.startingForCreate(),
          direct: Messages.startingForDirect(code),
          room: Messages.startingForRoom(),
        },
      };
      try {
        const result = await dolphinService.joinRoomInGame(code, ruleset);
        switch (result.outcome) {
          case "accepted":
            showInfo(text.accepted[kind]);
            return true;
          case "busy":
            showWarning(Messages.finishGameFirst());
            break;
          case "refused":
            showWarning(result.message || Messages.gameRefused());
            break;
          case "no-response":
            showWarning(text.noResponse[kind]);
            break;
          case "not-running": {
            const { id } = await dolphinService.prepareRoomLaunch(code, ruleset);
            if (await play()) {
              showInfo(text.starting[kind]);
              return true;
            }
            await dolphinService.cancelRoomLaunch(id);
            break;
          }
        }
      } catch (err) {
        log.error(err);
        showError(err);
      }
      return false;
    },
    [dolphinService, play, showError, showInfo, showWarning],
  );
};
