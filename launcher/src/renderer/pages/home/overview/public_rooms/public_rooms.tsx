import { useQuery } from "@tanstack/react-query";
import React from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
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
    />
  );
});
