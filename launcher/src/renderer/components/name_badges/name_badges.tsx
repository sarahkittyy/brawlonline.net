import type { BadgePlace, NameBadge } from "@common/badges";
import { badgesFor } from "@common/badges";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";

import { useIconUrl } from "@/lib/icons";

import styles from "./name_badges.module.css";

/** The most badges before a name in a list; the profile shows them all. */
export const MAX_BADGES_IN_LISTS = 3;

/**
 * The badges an admin gave a player (docs/design/profiles.md §10), before their name: those for `place`, in order,
 * each `size` px square with its hover text as a tooltip (as the old `</>` showed "Dev"). They never shrink, so a
 * long name is cut short ("…") and the badges stay whole. Nothing when there are none.
 */
export const NameBadges = ({
  badges,
  place,
  size = 16,
  max = MAX_BADGES_IN_LISTS,
  className,
}: {
  badges: readonly NameBadge[] | null | undefined;
  place: BadgePlace;
  size?: number;
  max?: number;
  className?: string;
}) => {
  const shown = badgesFor(badges, place).slice(0, max);
  if (shown.length === 0) {
    return null;
  }
  return (
    <span className={clsx(styles.badges, className)} data-testid="name-badges">
      {shown.map((b) => (
        <BadgeIcon key={b.icon} badge={b} size={size} />
      ))}
    </span>
  );
};

const BadgeIcon = ({ badge, size }: { badge: NameBadge; size: number }) => {
  const url = useIconUrl(badge.icon);
  const img = (
    <img
      className={styles.icon}
      src={url}
      alt={badge.label ?? ""}
      width={size}
      height={size}
      draggable={false}
      data-testid="name-badge"
    />
  );
  if (!badge.label) {
    return img;
  }
  return (
    <Tooltip title={badge.label} placement="top" enterDelay={200} disableInteractive={true}>
      {img}
    </Tooltip>
  );
};

/**
 * The icon an admin set in place of a player's primary main (docs/design/profiles.md §10), `size` px square, with
 * its hover text as a tooltip (as a badge's; a stock icon shows its character's name).
 */
export const MainIcon = ({
  icon,
  size,
  className,
  label,
}: {
  icon: string;
  size: number;
  className?: string;
  label?: string | null;
}) => {
  const url = useIconUrl(icon);
  const img = (
    <img
      className={clsx(styles.icon, className)}
      src={url}
      alt={label ?? ""}
      width={size}
      height={size}
      draggable={false}
      data-testid="main-icon"
    />
  );
  if (!label) {
    return img;
  }
  return (
    <Tooltip title={label} placement="top" enterDelay={200} disableInteractive={true}>
      {img}
    </Tooltip>
  );
};
