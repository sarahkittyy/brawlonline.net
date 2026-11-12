import { countryName } from "@common/profile";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { useQuery } from "@tanstack/react-query";
import React from "react";

import { DevBadge } from "@/components/dev_badge/dev_badge";
import { Flag } from "@/components/flag/flag";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { formatRating, HIDDEN_RATING } from "@/lib/ranked_format";
import { useServices } from "@/services";

import { CharacterIcon, useHasStockIcons } from "./character_icon";
import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";

/** Mains a card shows (the primary first); more are "+N". */
const CARD_MAINS = 8;

/**
 * A name's hover card (docs/design/profiles.md §8): after a short pause on the name, a small profile floats beside it
 * (badge, name, code, flag, rank and rating, mains). The profile is fetched the first time and kept a few minutes, as
 * the profile dialog's.
 */
export const MiniProfileTooltip = ({ uid, children }: { uid: string; children: React.ReactElement }) => (
  <Tooltip
    title={<MiniProfile uid={uid} />}
    enterDelay={450}
    enterNextDelay={450}
    placement="bottom-start"
    disableInteractive={true}
    slotProps={{ tooltip: { className: styles.miniTooltip } }}
  >
    {children}
  </Tooltip>
);

const MiniProfile = ({ uid }: { uid: string }) => {
  const { backendService } = useServices();
  const icons = useHasStockIcons();
  const { data } = useQuery({
    queryKey: ["profile", uid],
    queryFn: () => backendService.fetchProfile(uid),
    staleTime: 5 * 60_000,
  });
  if (!data) {
    return (
      <div className={styles.mini}>
        <CircularProgress size={16} />
      </div>
    );
  }
  const r = data.ranked;
  const placing = r.rank == null && r.setsPlayed > 0 && r.setsToPlace > 0;
  const mains = data.mains ?? [];
  return (
    <div className={styles.mini} data-testid="mini-profile">
      <div className={styles.miniTop}>
        {(r.rank || placing) && <RankBadge rank={r.rank} setsToPlace={placing ? r.setsToPlace : 0} size="small" />}
        <div className={styles.names}>
          <div className={styles.miniName}>
            {data.displayName}
            {data.dev && <DevBadge size={15} className={styles.miniDev} />}
          </div>
          <div className={styles.code}>{data.connectCode}</div>
          {data.country && (
            <div className={styles.miniCountry}>
              <Flag code={data.country} height={12} />
              {countryName(data.country)}
            </div>
          )}
        </div>
      </div>
      <div className={styles.miniRank}>
        {r.rank ? RankBadgeMessages.rankName(r.rank) : placing ? Messages.placing(r.setsToPlace) : Messages.notRanked()}
        {(r.rating != null || r.setsPlayed > 0) && (
          <span className={styles.miniRating}>{r.rating != null ? formatRating(r.rating) : HIDDEN_RATING}</span>
        )}
      </div>
      {mains.length > 0 && (
        <div className={icons ? styles.miniMains : styles.mainTags}>
          {mains.slice(0, CARD_MAINS).map((m, i) => (
            <CharacterIcon key={m} character={m} size={20} primary={i === 0} forceTag={!icons} />
          ))}
          {mains.length > CARD_MAINS && <span className={styles.none}>+{mains.length - CARD_MAINS}</span>}
        </div>
      )}
    </div>
  );
};
