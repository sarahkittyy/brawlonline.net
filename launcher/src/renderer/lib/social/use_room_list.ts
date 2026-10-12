import { useQuery } from "@tanstack/react-query";

import { useAccount } from "@/lib/hooks/use_account";
import { useServices } from "@/services";

/** How often the list is fetched while Home is shown (the server answers from a 1 s snapshot). */
export const ROOMS_POLL_MS = 4000;

/**
 * `GET /v1/rooms` (players online, the public rooms, the queues), polled while a user of it is
 * shown. Home's room list and its Play buttons share the one query.
 */
export const useRoomList = () => {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const query = useQuery({
    queryKey: ["publicRooms", uid],
    queryFn: () => backendService.fetchRooms(),
    enabled: uid != null,
    refetchInterval: ROOMS_POLL_MS,
    retry: false,
  });
  return { uid, query };
};
