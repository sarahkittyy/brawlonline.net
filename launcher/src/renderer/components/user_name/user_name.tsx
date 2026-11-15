import { countryName } from "@common/profile";
import { clsx } from "clsx";

import { Flag } from "@/components/flag/flag";
import { MiniProfileTooltip } from "@/components/profile/mini_profile";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { usePlayerMenuOpener } from "@/lib/social/use_player_menu";
import type { Rank } from "@/services/backend/types";

import { UserNameMessages as Messages } from "./user_name.messages";
import styles from "./user_name.module.css";

/**
 * What a name can show. Everything but the name is optional: a list that has a player's rank or flag passes it,
 * one that hasn't leaves it out (docs/design/profiles.md §5, §8).
 */
export type UserNameData = {
  uid?: string;
  displayName: string;
  /** `ABCD#123` */
  connectCode?: string;
  rank?: Rank | null;
  /** A flag code, when the player shows it beside their name. */
  country?: string | null;
};

/**
 * A player's name, as the launcher shows it everywhere (the online list, chat, rooms, the leaderboard, the match
 * history): the rank badge and the flag before it, the connect code after it, lighter. Each part is switched on
 * where it's wanted. `profileLink` makes the name a button that opens the player's profile (View Profile); with a
 * uid, hovering the name shows a small profile card (`hoverCard`, on by default) and right-clicking it opens the
 * online list's player menu (`playerMenu`, on by default with the hover card; off where the row has its own menu).
 *
 * `classes` restyles the parts for a place with its own look (the chat's names are its own CSS).
 */
export const UserName = ({
  user,
  showCode = true,
  showBadge = false,
  showFlag = false,
  profileLink = false,
  hoverCard = true,
  playerMenu = true,
  menuMessage = true,
  className,
  classes,
}: {
  user: UserNameData;
  /** The connect code after the name. */
  showCode?: boolean;
  /**
   * The rank badge before the name; a badge-wide gap when the player has none (placing, unranked), so the
   * names in a list line up.
   */
  showBadge?: boolean;
  /** The player's flag before the name, when they show one. */
  showFlag?: boolean;
  /** Clicking the name opens the player's profile (needs `uid`). */
  profileLink?: boolean;
  /** Hovering the name shows a small profile card (needs `uid`). */
  hoverCard?: boolean;
  /** Right-clicking the name opens the player menu (with the hover card, needs `uid`). */
  playerMenu?: boolean;
  /** The menu offers Message (not in that player's own chat window). */
  menuMessage?: boolean;
  className?: string;
  classes?: { name?: string; code?: string };
}) => {
  const openProfile = useProfileView((s) => s.open);
  const uid = user.uid;
  const onContextMenu = usePlayerMenuOpener(user, { showMessage: menuMessage, enabled: hoverCard && playerMenu });
  let name = (
    <span className={clsx(styles.name, classes?.name)} onContextMenu={onContextMenu}>
      {user.displayName}
    </span>
  );
  if (profileLink && uid) {
    name = (
      <button
        type="button"
        className={styles.link}
        data-profile-link={uid}
        onClick={(e) => {
          e.stopPropagation();
          openProfile({ uid, displayName: user.displayName, connectCode: user.connectCode });
        }}
      >
        {name}
      </button>
    );
  }
  if (hoverCard && uid) {
    name = <MiniProfileTooltip uid={uid}>{name}</MiniProfileTooltip>;
  } else if (profileLink && uid) {
    name = <span title={Messages.viewProfile()}>{name}</span>;
  }
  return (
    <span className={clsx(styles.root, className)}>
      {showBadge && (
        <span className={styles.badge}>
          <RankBadge rank={user.rank} size="inline" outlineWhenNone={true} />
        </span>
      )}
      {showFlag && user.country && (
        <Flag code={user.country} height={12} title={countryName(user.country)} className={styles.flag} />
      )}
      <span className={styles.text}>
        {name}
        {showCode && user.connectCode && <span className={clsx(styles.code, classes?.code)}>{user.connectCode}</span>}
      </span>
    </span>
  );
};
