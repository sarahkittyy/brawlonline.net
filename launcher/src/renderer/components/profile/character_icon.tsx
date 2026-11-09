import { characterName } from "@common/profile";
import { clsx } from "clsx";

import { assetUrl, useGameAssets } from "@/styles/game_theme";

import styles from "./profile.module.css";

/** The character's stock icon from the player's own disc and P+ card, or undefined before the assets are extracted. */
export function useStockIcon(key: string): string | undefined {
  const file = useGameAssets((s) => s.state?.manifest?.stocks[key]?.file);
  return file ? assetUrl(file) : undefined;
}

/** Whether the extracted assets have stock icons (a disc is set up); without them characters show as name tags. */
export function useHasStockIcons(): boolean {
  return useGameAssets((s) => Object.keys(s.state?.manifest?.stocks ?? {}).length > 0);
}

/**
 * A character: its stock icon `size` px square, or (no disc, or no icon for it) a rounded tag with its name.
 * `primary` marks the primary main.
 */
export const CharacterIcon = ({
  character,
  size,
  primary = false,
  forceTag = false,
}: {
  character: string;
  size: number;
  primary?: boolean;
  /** A tag even with an icon (no disc: every character a tag, so they look alike). */
  forceTag?: boolean;
}) => {
  const url = useStockIcon(character);
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
