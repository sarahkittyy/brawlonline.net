import type { RoomKind } from "@accounts/types";
import { useQueryClient } from "@tanstack/react-query";
import { useCallback, useState } from "react";

import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

/** How long a new name is shown before the room list has it: mm reads it with its account poll (3 s), then the
 * list's snapshot (1.5 s) and its poll (4 s) bring it here. */
const PENDING_MS = 15_000;

/**
 * Naming the room the player hosts (docs/design/rooms.md §1.4): `rename` stores the name for the player's rooms of
 * this kind on the server, which names the room they host now and their next ones. `shown` gives a room's name with
 * the one just stored, until the room list has caught up.
 */
export const useRoomName = (kind: RoomKind) => {
  const { backendService } = useServices();
  const { showError } = useToasts();
  const queryClient = useQueryClient();
  const [pending, setPending] = useState<{ code: string; name: string | null; at: number } | null>(null);

  /** Stores `typed` (empty: no name) for room `code`; throws (after a toast) when the server refused it. */
  const rename = useCallback(
    async (code: string, typed: string) => {
      try {
        // What was stored: cleaned up, or a kind name in place of a slur.
        const stored = await backendService.setRoomName(kind, typed.trim() || null);
        setPending({ code, name: stored, at: Date.now() });
        void queryClient.invalidateQueries({ queryKey: ["publicRooms"] });
      } catch (err) {
        showError(err);
        throw err;
      }
    },
    [backendService, kind, queryClient, showError],
  );

  const shown = useCallback(
    (code: string, listed: string | null | undefined): string | null => {
      if (pending && pending.code === code && Date.now() - pending.at < PENDING_MS) {
        return pending.name;
      }
      return listed ?? null;
    },
    [pending],
  );

  return { rename, shown };
};
