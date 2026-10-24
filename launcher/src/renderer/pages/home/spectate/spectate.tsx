import type { LiveGameEntry } from "@spectate/types";
import React, { useEffect, useRef, useState } from "react";

import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useLiveGames, useNow, useSpectateState } from "@/lib/spectate/use_spectate";

import { SpectateMessages as Messages } from "./spectate.messages";
import styles from "./spectate.module.css";
import type { LiveSort, SpectateListState } from "./spectate_view";
import { SpectateView } from "./spectate_view";

/** How long a list stays shown while polls fail (as Public Rooms). */
const STALE_AFTER_MS = 15_000;
/** Auto play, remembered on this computer (off by default, the user 2026-10-13). */
const AUTO_PLAY_STORAGE_KEY = "SPECTATE_AUTO_PLAY";
/** The list's sort, remembered on this computer. Only the list: Auto play still picks the newest live game. */
const SORT_STORAGE_KEY = "SPECTATE_SORT";

/** Home > Spectate (docs/design/spectating.md §6.1): the live games, polled every 3 s while the tab is open. */
export const Spectate = React.memo(function Spectate() {
  const { uid, query } = useLiveGames();
  const openLogin = useLoginModal((s) => s.openModal);
  const { showError } = useToasts();
  const watching = useSpectateState();
  const now = useNow(1000);
  const [autoPlay, setAutoPlay] = useLocalStorage(AUTO_PLAY_STORAGE_KEY, false);
  const [sort, setSort] = useLocalStorage<LiveSort>(SORT_STORAGE_KEY, "newest");
  const [startingId, setStartingId] = useState<string | undefined>();
  const freshIds = useFreshIds(query.data?.games);

  // The main process keeps Auto play for the stream's end (also when this tab isn't showing).
  useEffect(() => {
    void window.electron.spectate.setAutoPlay(autoPlay);
  }, [autoPlay]);

  const watch = async (game: LiveGameEntry) => {
    if (!uid) {
      return;
    }
    setStartingId(game.id);
    try {
      const result = await window.electron.spectate.watch(uid, game.id);
      if (!result.ok) {
        showError(Messages.cantWatch(result.message));
        void query.refetch();
      }
    } catch (err) {
      showError(Messages.cantWatch(err instanceof Error ? err.message : String(err)));
    } finally {
      setStartingId(undefined);
    }
  };

  const outdated = query.isError && Date.now() - query.dataUpdatedAt > STALE_AFTER_MS;
  let state: SpectateListState;
  if (!uid) {
    state = { kind: "logged-out" };
  } else if (query.data && !outdated) {
    state = { kind: "ready", games: query.data.games };
  } else if (query.isError) {
    state = { kind: "unreachable" };
  } else {
    state = { kind: "loading" };
  }

  return (
    <div className={styles.page}>
      <SpectateView
        state={state}
        uid={uid}
        watching={watching}
        startingId={startingId}
        autoPlay={autoPlay}
        onAutoPlayChange={setAutoPlay}
        onWatch={(game) => void watch(game)}
        onRetry={() => void query.refetch()}
        onLogIn={openLogin}
        now={now}
        freshIds={freshIds}
        sort={sort === "elo" ? "elo" : "newest"}
        onSortChange={setSort}
      />
    </div>
  );
});

/** The games that appeared after the first list this tab got. */
function useFreshIds(games: readonly LiveGameEntry[] | undefined): ReadonlySet<string> {
  const seen = useRef<Set<string> | null>(null);
  const [fresh, setFresh] = useState<ReadonlySet<string>>(new Set());
  useEffect(() => {
    if (!games) {
      return;
    }
    if (seen.current === null) {
      seen.current = new Set(games.map((g) => g.id));
      return;
    }
    const added = games.filter((g) => !seen.current!.has(g.id)).map((g) => g.id);
    if (added.length > 0) {
      added.forEach((id) => seen.current!.add(id));
      setFresh(new Set(added));
    }
  }, [games]);
  return fresh;
}
