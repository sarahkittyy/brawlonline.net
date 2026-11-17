import { MAX_BADGES } from "@common/badges";
import { countryName } from "@common/profile";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { useQuery } from "@tanstack/react-query";
import React from "react";

import { Flag } from "@/components/flag/flag";
import { MainIcon, NameBadges } from "@/components/name_badges/name_badges";
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
 * (badge, the admin-given badges and name, code, flag, rank and rating, mains with an admin's custom main icon first). The profile is fetched the first time and kept a few minutes, as
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
  const mainIcon = data.mainIcon ?? null;
  const shownMains = mainIcon ? CARD_MAINS - 1 : CARD_MAINS;
  return (
    <div className={styles.mini} data-testid="mini-profile">
      <div className={styles.miniTop}>
        {(r.rank || placing) && <RankBadge rank={r.rank} setsToPlace={placing ? r.setsToPlace : 0} size="small" />}
        <div className={styles.names}>
          <div className={styles.miniName}>
            <NameBadges badges={data.badges} place="hover" size={15} max={MAX_BADGES} className={styles.miniBadges} />
            {data.displayName}
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
      {(mains.length > 0 || mainIcon) && (
        <div className={icons ? styles.miniMains : styles.mainTags}>
          {/* An admin's custom main icon is the primary, before the player's own (docs/design/profiles.md §10). */}
          {mainIcon && <MainIcon icon={mainIcon} label={data.mainIconLabel} size={20} />}
          {mains.slice(0, shownMains).map((m, i) => (
            <CharacterIcon key={m} character={m} size={20} primary={i === 0 && !mainIcon} forceTag={!icons} />
          ))}
          {mains.length > shownMains && <span className={styles.none}>+{mains.length - shownMains}</span>}
        </div>
      )}
    </div>
  );
};
