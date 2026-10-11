import { CREATE_ROOM } from "@dolphin/hand_off_target";
import { useQuery } from "@tanstack/react-query";
import React, { useState } from "react";

import { SocialMessages } from "@/components/social/social.messages";
import { useAccount } from "@/lib/hooks/use_account";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useHandOff } from "@/lib/social/use_hand_off";
import { useSocial } from "@/lib/social/use_social";
import { useServices } from "@/services";

import type { PublicRoomsState } from "./public_rooms_view";
import { PublicRoomsView } from "./public_rooms_view";
import { useJoinRoom } from "./use_join_room";

/** How often the list is fetched while Home is shown (the server answers from a 1 s snapshot). */
export const ROOMS_POLL_MS = 4000;
/** How long a list stays shown while polls fail. */
const STALE_AFTER_MS = 15_000;

/** Players online and the public rooms, polled every few seconds while Home > Overview is open. */
export const PublicRooms = React.memo(function PublicRooms() {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const openLogin = useLoginModal((s) => s.openModal);
  const { join, joiningCode } = useJoinRoom();
  const handOff = useHandOff();
  const { showWarning } = useToasts();
  const [creating, setCreating] = useState(false);
  const create = () => {
    setCreating(true);
    // Invites sent before the room shows up wait for it (SocialRoot sends them).
    useSocial.getState().startPendingRoom();
    void handOff(CREATE_ROOM)
      .then((taken) => {
        if (!taken) {
          const waiting = useSocial.getState().endPendingRoom();
          if (waiting.length > 0) {
            showWarning(SocialMessages.roomDidNotOpen(waiting.map((p) => p.displayName).join(", ")));
          }
        }
      })
      .finally(() => setCreating(false));
  };

  const query = useQuery({
    queryKey: ["publicRooms", uid],
    queryFn: () => backendService.fetchRooms(),
    enabled: uid != null,
    refetchInterval: ROOMS_POLL_MS,
    retry: false,
  });

  // A failed poll keeps the last list for a little while (the next poll usually replaces it);
  // after that the list is too old to click on.
  const outdated = query.isError && Date.now() - query.dataUpdatedAt > STALE_AFTER_MS;
  let state: PublicRoomsState;
  if (!uid) {
    state = { kind: "logged-out" };
  } else if (query.data && !outdated) {
    state = { kind: "ready", online: query.data.online, rooms: query.data.rooms };
  } else if (query.isError) {
    state = { kind: "unreachable" };
  } else {
    state = { kind: "loading" };
  }

  return (
    <PublicRoomsView
      state={state}
      joiningCode={joiningCode}
      onJoin={(room) => void join(room)}
      onRetry={() => void query.refetch()}
      onLogIn={openLogin}
      onCreate={create}
      creating={creating}
    />
  );
});
