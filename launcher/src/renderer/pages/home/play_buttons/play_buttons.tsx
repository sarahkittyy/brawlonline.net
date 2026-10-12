import type { QueueTarget } from "@dolphin/hand_off_target";
import React, { useState } from "react";

import { useHandOff } from "@/lib/social/use_hand_off";
import { useRoomList } from "@/lib/social/use_room_list";

import { PlayButtonsView } from "./play_buttons_view";

/**
 * Play unranked / Play ranked: the game goes to that mode's character select, started first when
 * it isn't running (a `queue` hand-off, docs/rooms-protocol.md §5). The counts come from the room
 * list's poll (`GET /v1/rooms` `queues` and `playing`).
 */
export const PlayButtons = React.memo(function PlayButtons() {
  const { uid, query } = useRoomList();
  const handOff = useHandOff();
  const [starting, setStarting] = useState<QueueTarget>();

  const play = async (queue: QueueTarget) => {
    setStarting(queue);
    try {
      await handOff(queue);
    } finally {
      setStarting(undefined);
    }
  };

  const known = uid && !query.isError;
  const queues = known ? query.data?.queues : undefined;
  const playing = known ? query.data?.playing : undefined;
  return <PlayButtonsView queues={queues} playing={playing} starting={starting} onPlay={(q) => void play(q)} />;
});
