import type { Rank, RankTier } from "@/services/backend/types";

import { RankBadge } from "./rank_badge";
import { RankBadgeMessages } from "./rank_badge.messages";

export default {
  title: "components/RankBadge",
  argTypes: {},
};

const TIERS: RankTier[] = ["bronze", "silver", "gold", "emerald", "diamond", "master"];

/** Every rank, lowest first: each tier's divisions 3, 2, 1, then Legendary. */
const ALL: Rank[] = [
  ...TIERS.flatMap((tier) => ([3, 2, 1] as const).map((division) => ({ tier, division }))),
  { tier: "legendary", division: null },
];

/** The story's own label under each badge (the launcher shows the name only on hover). */
const Captioned = ({ caption, children }: { caption: string; children: React.ReactNode }) => (
  <div style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 6, minWidth: 90 }}>
    {children}
    <span style={{ fontSize: 11, color: "#8a8a8a" }}>{caption}</span>
  </div>
);

const Grid = ({ size }: { size: "large" | "small" }) => (
  <div style={{ background: "var(--surface-1, #1f1f1f)", padding: 24 }}>
    <div style={{ display: "grid", gridTemplateColumns: "repeat(6, max-content)", gap: size === "large" ? 28 : 18 }}>
      {ALL.map((rank) => (
        <Captioned key={`${rank.tier}-${rank.division}`} caption={RankBadgeMessages.rankName(rank)}>
          <RankBadge rank={rank} size={size} />
        </Captioned>
      ))}
      <Captioned caption="Placing, 3 left">
        <RankBadge setsToPlace={3} size={size} />
      </Captioned>
      <Captioned caption="Placing, 1 left">
        <RankBadge setsToPlace={1} size={size} />
      </Captioned>
    </div>
  </div>
);

export const Large = () => <Grid size="large" />;
export const Small = () => <Grid size="small" />;
