import { CharacterIcon, useStockIcon } from "@/components/profile/character_icon";
import { FriendsIcon } from "@/components/social/friend_icons";
import { SocialMessages as Messages } from "@/components/social/social.messages";

import styles from "./online_players.module.css";

/**
 * A player's primary main before their name (docs/design/profiles.md §8): its stock icon, or a small
 * dot in the theme accent's hue for a player without one shown (no main, "Show next to name" off, or a
 * character this disc has no stock icon for), so names line up without a hole or a stand-in character.
 * A friend (docs/design/friends.md): the friends glyph at the icon's bottom-right corner, or in the
 * dot's place.
 */
export const MainSlot = ({ main, friend = false }: { main?: string; friend?: boolean }) => {
  const url = useStockIcon(main ?? "");
  const hasMain = main != null && (main === "random" || url != null);
  return (
    <span className={styles.main} data-testid="online-main" style={friend ? { position: "relative" } : undefined}>
      {hasMain ? (
        <CharacterIcon character={main} size={20} />
      ) : friend ? (
        <FriendsIcon className={styles.friendGlyph} style={{ fontSize: 14 }} data-testid="online-friend" />
      ) : (
        <span className={styles.noMain} />
      )}
      {hasMain && friend && (
        <span className={styles.friendBadge} title={Messages.friend()} data-testid="online-friend">
          <FriendsIcon style={{ fontSize: 10 }} />
        </span>
      )}
    </span>
  );
};
