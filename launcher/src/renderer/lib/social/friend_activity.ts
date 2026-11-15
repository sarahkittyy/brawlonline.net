import type { PublicRoom } from "@accounts/types";
import type { LiveGameEntry } from "@spectate/types";
import { useQuery } from "@tanstack/react-query";

import { useAccount } from "@/lib/hooks/use_account";

/**
 * What a friend is doing, as the Friends tab shows it (docs/design/friends.md): in a room the player
 * can see (public, or friends only), playing a game others can watch, or just online. Private rooms
 * stay private: a friend in one is "online".
 */
export type FriendActivity =
  | { kind: "room"; room: PublicRoom; yours: boolean }
  | { kind: "game"; game: LiveGameEntry }
  | { kind: "online" };

export function activityOf(
  uid: string,
  rooms: PublicRoom[] | undefined,
  yourRoomCode: string | undefined,
  live: LiveGameEntry[] | undefined,
): FriendActivity {
  const room = rooms?.find((r) => r.uids?.includes(uid));
  if (room) {
    return { kind: "room", room, yours: room.code === yourRoomCode };
  }
  const game = live?.find((g) => g.game.state !== "over" && g.players.some((p) => p.uid === uid));
  if (game) {
    return { kind: "game", game };
  }
  return { kind: "online" };
}

/** How often the live games are read while the Friends tab is shown (the Spectate tab reads them every 3 s). */
export const FRIENDS_LIVE_POLL_MS = 10_000;

/** `GET /v1/live` for the Friends tab: which friends play a game others can watch. */
export const useFriendsLiveGames = (enabled: boolean) => {
  const uid = useAccount((s) => s.user?.uid);
  return useQuery({
    queryKey: ["liveGames", uid, "friends"],
    queryFn: () => window.electron.spectate.list(uid!),
    enabled: enabled && uid != null,
    refetchInterval: FRIENDS_LIVE_POLL_MS,
    retry: false,
  });
};
