import type { QueueTarget } from "@dolphin/hand_off_target";
import React, { useState } from "react";

import { useHandOff } from "@/lib/social/use_hand_off";

import { PlayButtonsView } from "./play_buttons_view";

/**
 * Play unranked / Play ranked / Play 2v2: the game goes to that mode's character select, started first when
 * it isn't running (a `queue` hand-off, docs/rooms-protocol.md §5).
 */
export const PlayButtons = React.memo(function PlayButtons() {
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

  return <PlayButtonsView starting={starting} onPlay={(q) => void play(q)} />;
});
