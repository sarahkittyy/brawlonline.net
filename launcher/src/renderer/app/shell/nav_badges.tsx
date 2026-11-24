import { TwitchGlyph } from "@/components/twitch/twitch";
import { useRoomList } from "@/lib/social/use_room_list";
import { useLiveGames } from "@/lib/spectate/use_spectate";
import { useTwitchStreams } from "@/lib/twitch/use_twitch";

import { NavBadge } from "./sidebar";
import styles from "./sidebar.module.css";

/** Away from their pages the sidebar's counts are read slowly; their own pages poll faster (the query is shared). */
const BADGE_POLL_MS = 20_000;

/** Other Games' public rooms. */
export const OtherGamesBadge = () => {
  const count = useRoomList(BADGE_POLL_MS).query.data?.modRooms?.length;
  return count ? <NavBadge>{count}</NavBadge> : null;
};

/**
 * Spectate's live games, after a red dot; before them Twitch's logo while players stream Brawl Online (Spectate's
 * Twitch row, docs/design/twitch-live.md).
 */
export const SpectateBadge = () => {
  const count = useLiveGames(BADGE_POLL_MS).query.data?.games.length;
  const streaming = useTwitchStreams().length > 0;
  if (!count && !streaming) {
    return null;
  }
  return (
    <span className={styles.badges}>
      {streaming && <TwitchGlyph size={14} />}
      {count ? <NavBadge live={true}>{count}</NavBadge> : null}
    </span>
  );
};
