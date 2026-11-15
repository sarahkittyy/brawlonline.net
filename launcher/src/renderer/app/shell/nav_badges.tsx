import { useRoomList } from "@/lib/social/use_room_list";
import { useLiveGames } from "@/lib/spectate/use_spectate";

import { NavBadge } from "./sidebar";

/** Away from their pages the sidebar's counts are read slowly; their own pages poll faster (the query is shared). */
const BADGE_POLL_MS = 20_000;

/** Other Games' public rooms. */
export const OtherGamesBadge = () => {
  const count = useRoomList(BADGE_POLL_MS).query.data?.modRooms?.length;
  return count ? <NavBadge>{count}</NavBadge> : null;
};

/** Spectate's live games, after a red dot. */
export const SpectateBadge = () => {
  const count = useLiveGames(BADGE_POLL_MS).query.data?.games.length;
  return count ? <NavBadge live={true}>{count}</NavBadge> : null;
};
