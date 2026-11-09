import { clsx } from "clsx";

import { RankBadge } from "@/components/rank_badge/rank_badge";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import type { Rank } from "@/services/backend/types";

import { UserNameMessages as Messages } from "./user_name.messages";
import styles from "./user_name.module.css";

/**
 * What a name can show. Everything but the name is optional: a list that has a player's rank passes it,
 * one that hasn't leaves it out. Profile fields added later (a country flag, mains, a tag;
 * docs/design/profiles.md) go here too, each with its own `show` switch below.
 */
export type UserNameData = {
  uid?: string;
  displayName: string;
  /** `ABCD#123` */
  connectCode?: string;
  rank?: Rank | null;
};

/**
 * A player's name, as the launcher shows it everywhere (the online list, chat, the match history): the rank
 * badge before it, the connect code after it, lighter. Each part is switched on where it's wanted.
 * `profileLink` makes the name a button that opens the player's profile (View Profile).
 *
 * `classes` restyles the parts for a place with its own look (the chat's names are its own CSS).
 */
export const UserName = ({
  user,
  showCode = true,
  showBadge = false,
  profileLink = false,
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
  /** Clicking the name opens the player's profile (needs `uid`). */
  profileLink?: boolean;
  className?: string;
  classes?: { name?: string; code?: string };
}) => {
  const openProfile = useProfileView((s) => s.open);
  const uid = user.uid;
  const name = <span className={clsx(styles.name, classes?.name)}>{user.displayName}</span>;
  return (
    <span className={clsx(styles.root, className)}>
      {showBadge && (
        <span className={styles.badge}>
          <RankBadge rank={user.rank} size="inline" />
        </span>
      )}
      <span className={styles.text}>
        {profileLink && uid ? (
          <button
            type="button"
            className={styles.link}
            title={Messages.viewProfile()}
            onClick={(e) => {
              e.stopPropagation();
              openProfile({ uid, displayName: user.displayName, connectCode: user.connectCode });
            }}
          >
            {name}
          </button>
        ) : (
          name
        )}
        {showCode && user.connectCode && <span className={clsx(styles.code, classes?.code)}>{user.connectCode}</span>}
      </span>
    </span>
  );
};
