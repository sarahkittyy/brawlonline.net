import { characterName } from "@common/profile";
import { clsx } from "clsx";

import { MainIcon } from "@/components/name_badges/name_badges";
import { useIconUrl } from "@/lib/icons";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

import styles from "./profile.module.css";

/** The character's stock icon from the player's own disc and P+ card, or undefined before the assets are extracted. */
export function useStockIcon(key: string): string | undefined {
  const file = useGameAssets((s) => s.state?.manifest?.stocks[key]?.file);
  return file ? assetUrl(file) : undefined;
}

/**
 * A player's picture by their primary main: the icon an admin set in place of it (docs/design/profiles.md §10), else
 * the main's stock icon; undefined for neither (or before the assets are extracted).
 */
export function useMainPictureUrl(
  main: string | null | undefined,
  mainIcon: string | null | undefined,
): string | undefined {
  const stock = useStockIcon(main ?? "");
  const custom = useIconUrl(mainIcon);
  return custom ?? (main ? stock : undefined);
}

/** Whether the extracted assets have stock icons (a disc is set up); without them characters show as name tags. */
export function useHasStockIcons(): boolean {
  return useGameAssets((s) => Object.keys(s.state?.manifest?.stocks ?? {}).length > 0);
}

/**
 * A character: its stock icon `size` px square, or (no disc, or no icon for it) a rounded tag with its name.
 * `primary` marks the primary main. With `icon` (a player's primary main beside their name), the icon an admin set
 * in its place instead (docs/design/profiles.md §10).
 */
export const CharacterIcon = ({
  character,
  size,
  primary = false,
  forceTag = false,
  icon,
  iconLabel,
}: {
  character: string;
  size: number;
  primary?: boolean;
  /** A tag even with an icon (no disc: every character a tag, so they look alike). */
  forceTag?: boolean;
  /** The custom main icon's id: shown instead of the character. */
  icon?: string | null;
  /** The custom main icon's hover text. */
  iconLabel?: string | null;
}) => {
  const url = useStockIcon(character);
  if (icon) {
    return <MainIcon icon={icon} label={iconLabel} size={size} />;
  }
  const name = characterName(character) ?? character;
  const title = primary ? `${name} (primary)` : name;
  // Random: a "?" alone, centered in the stock icon's square (Brawl has no "?" stock icon).
  if (character === "random" && !forceTag) {
    return (
      <svg
        className={styles.randomIcon}
        viewBox="0 0 32 32"
        width={size}
        height={size}
        role="img"
        aria-label={name}
        data-testid="stock-random"
      >
        <title>{title}</title>
        <text x="16" y="16" textAnchor="middle" dominantBaseline="central">
          ?
        </text>
      </svg>
    );
  }
  if (!url || forceTag) {
    return (
      <span className={clsx(styles.tag, primary && styles.primaryTag)} title={title}>
        {name}
      </span>
    );
  }
  return (
    <img
      className={styles.stock}
      src={url}
      alt={name}
      title={title}
      width={size}
      height={size}
      draggable={false}
      data-testid={`stock-${character}`}
    />
  );
};
