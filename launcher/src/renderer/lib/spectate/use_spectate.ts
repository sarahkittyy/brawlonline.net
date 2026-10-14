import type { SpectateState } from "@spectate/types";
import { useQuery } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { useAccount } from "@/lib/hooks/use_account";

/** How often the live games are fetched while the Spectate tab is open (the server answers from a 1 s snapshot). */
export const LIVE_POLL_MS = 3000;

/** `GET /v1/live`, polled while the Spectate tab is shown (docs/design/spectating.md §6.1). */
export const useLiveGames = () => {
  const uid = useAccount((s) => s.user?.uid);
  const query = useQuery({
    queryKey: ["liveGames", uid],
    queryFn: () => window.electron.spectate.list(uid!),
    enabled: uid != null,
    refetchInterval: LIVE_POLL_MS,
    retry: false,
  });
  return { uid, query };
};

const IDLE: SpectateState = {
  status: "idle",
  stream: null,
  entry: null,
  spectators: 0,
  autoPlay: false,
  waitingForGame: false,
};

/** What the launcher is watching (the main process's SpectateClient). */
export const useSpectateState = (): SpectateState => {
  const [state, setState] = useState<SpectateState>(IDLE);
  useEffect(() => {
    let alive = true;
    void window.electron.spectate.getState().then((s) => alive && setState(s));
    const off = window.electron.spectate.onStateChange((s) => setState(s));
    return () => {
      alive = false;
      off();
    };
  }, []);
  return state;
};

/** The time now, every `ms` (a row's game time ticks between polls). */
export const useNow = (ms = 1000): number => {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(t);
  }, [ms]);
  return now;
};
