import Tooltip from "@mui/material/Tooltip";

import type { Rank, RankTier } from "@/services/backend/types";

import bronze1 from "./images/bronze-1.png";
import bronze2 from "./images/bronze-2.png";
import bronze3 from "./images/bronze-3.png";
import diamond1 from "./images/diamond-1.png";
import diamond2 from "./images/diamond-2.png";
import diamond3 from "./images/diamond-3.png";
import emerald1 from "./images/emerald-1.png";
import emerald2 from "./images/emerald-2.png";
import emerald3 from "./images/emerald-3.png";
import gold1 from "./images/gold-1.png";
import gold2 from "./images/gold-2.png";
import gold3 from "./images/gold-3.png";
import legendary from "./images/legendary.png";
import master1 from "./images/master-1.png";
import master2 from "./images/master-2.png";
import master3 from "./images/master-3.png";
import silver1 from "./images/silver-1.png";
import silver2 from "./images/silver-2.png";
import silver3 from "./images/silver-3.png";
import bronze1S from "./images/small/bronze-1.png";
import bronze2S from "./images/small/bronze-2.png";
import bronze3S from "./images/small/bronze-3.png";
import diamond1S from "./images/small/diamond-1.png";
import diamond2S from "./images/small/diamond-2.png";
import diamond3S from "./images/small/diamond-3.png";
import emerald1S from "./images/small/emerald-1.png";
import emerald2S from "./images/small/emerald-2.png";
import emerald3S from "./images/small/emerald-3.png";
import gold1S from "./images/small/gold-1.png";
import gold2S from "./images/small/gold-2.png";
import gold3S from "./images/small/gold-3.png";
import legendaryS from "./images/small/legendary.png";
import master1S from "./images/small/master-1.png";
import master2S from "./images/small/master-2.png";
import master3S from "./images/small/master-3.png";
import silver1S from "./images/small/silver-1.png";
import silver2S from "./images/small/silver-2.png";
import silver3S from "./images/small/silver-3.png";
import { RankBadgeMessages as Messages } from "./rank_badge.messages";
import styles from "./rank_badge.module.css";

// Layerlab's 2D League Icon Pack (bought by the user, 2026-10-14) with each division's numeral baked onto a
// plaque at the foot (procedurally, docs/design/ranked-tiers.md): 256 px with Roman numerals for the large
// badge, 128 px with a bigger plaque and plain bars for the small one (letters blur at 40 px).
type Division = 1 | 2 | 3;
const EMBLEMS: Record<"large" | "small", Record<Exclude<RankTier, "legendary">, Record<Division, string>>> = {
  large: {
    bronze: { 1: bronze1, 2: bronze2, 3: bronze3 },
    silver: { 1: silver1, 2: silver2, 3: silver3 },
    gold: { 1: gold1, 2: gold2, 3: gold3 },
    emerald: { 1: emerald1, 2: emerald2, 3: emerald3 },
    diamond: { 1: diamond1, 2: diamond2, 3: diamond3 },
    master: { 1: master1, 2: master2, 3: master3 },
  },
  small: {
    bronze: { 1: bronze1S, 2: bronze2S, 3: bronze3S },
    silver: { 1: silver1S, 2: silver2S, 3: silver3S },
    gold: { 1: gold1S, 2: gold2S, 3: gold3S },
    emerald: { 1: emerald1S, 2: emerald2S, 3: emerald3S },
    diamond: { 1: diamond1S, 2: diamond2S, 3: diamond3S },
    master: { 1: master1S, 2: master2S, 3: master3S },
  },
};

function emblemFor(rank: Rank, size: "large" | "small"): string {
  if (rank.tier === "legendary" || !rank.division) {
    return size === "large" ? legendary : legendaryS;
  }
  return EMBLEMS[size][rank.tier][rank.division];
}

// The emblems' common silhouette (traced from the Gold one's alpha, 256 x 256), drawn dotted while placing.
const OUTLINE =
  "M123 9 L112 16 L94 34 L49 35 L32 39 L27 46 L26 64 L17 69 L16 81 L22 116 L27 121 L30 174 L36 191 L52 207 " +
  "L75 223 L116 247 L131 249 L142 245 L194 213 L209 201 L220 188 L225 165 L227 124 L236 116 L240 97 L242 72 " +
  "L238 66 L228 65 L227 46 L222 39 L203 35 L159 34 L136 12 L130 9 Z";

/**
 * A ranked emblem, its division shown by the numeral on it; hovering names the rank ("Gold 2"). While the
 * player is still placing, a dotted outline with "3 more sets" under it. Nothing without a rank (an older
 * server).
 */
export const RankBadge = ({
  rank,
  setsToPlace,
  size,
}: {
  rank?: Rank | null;
  setsToPlace?: number;
  size: "large" | "small";
}) => {
  const sizeClass = size === "large" ? styles.large : styles.small;
  if (setsToPlace && setsToPlace > 0) {
    return (
      <Tooltip title={Messages.placingTooltip(setsToPlace)} arrow={true}>
        <div className={`${styles.badge} ${sizeClass}`} data-tier="placing">
          <svg className={styles.emblem} viewBox="0 0 256 256" aria-hidden={true}>
            <path className={styles.outline} d={OUTLINE} />
          </svg>
          <span className={styles.tag}>
            {size === "large" ? Messages.setsToPlace(setsToPlace) : Messages.setsToPlaceShort(setsToPlace)}
          </span>
        </div>
      </Tooltip>
    );
  }
  if (!rank) {
    return null;
  }
  const name = Messages.rankName(rank);
  return (
    <Tooltip title={name} arrow={true}>
      <div className={`${styles.badge} ${sizeClass}`} data-tier={rank.tier}>
        <img className={styles.emblem} src={emblemFor(rank, size)} alt={name} draggable={false} />
      </div>
    </Tooltip>
  );
};
