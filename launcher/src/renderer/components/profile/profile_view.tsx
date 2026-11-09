import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import OpenInNewIcon from "@mui/icons-material/OpenInNew";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { RankBadge } from "@/components/rank_badge/rank_badge";
import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { formatPosition, formatRating, HIDDEN_RATING } from "@/lib/ranked_format";
import type { PlayerProfile } from "@/services/backend/types";

import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";

/**
 * A player's profile (docs/design/profiles.md): the badge, name, code and rank, then the Ranked numbers,
 * then the profile's web page. The website's `/profile/<code>` page (server/crates/accounts/src/profile.rs)
 * shows the same fields in the same order; a field added to one goes in the other.
 *
 * `actions` are the buttons under it (Message, Ignore user), none for the player's own profile.
 */
export const ProfileView = React.memo(function ProfileView({
  profile,
  actions,
  onCopyLink,
}: {
  profile: PlayerProfile;
  actions?: React.ReactNode;
  onCopyLink?: (link: string) => void;
}) {
  const r = profile.ranked;
  const placing = r.rank == null && r.setsPlayed > 0 && r.setsToPlace > 0;
  const standing = r.rank
    ? RankBadgeMessages.rankName(r.rank)
    : placing
    ? Messages.placing(r.setsToPlace)
    : Messages.notRanked();
  const position = formatPosition(r.position, r.rankedPlayers);
  return (
    <div className={styles.profile} data-testid="profile-view">
      <div className={styles.who} data-tier={r.rank?.tier ?? "none"}>
        {(r.rank || placing) && (
          <div className={styles.badge}>
            <RankBadge rank={r.rank} setsToPlace={placing ? r.setsToPlace : 0} size="large" />
          </div>
        )}
        <div className={styles.names}>
          <div className={styles.name} data-testid="profile-name">
            {profile.displayName}
          </div>
          {profile.connectCode && <div className={styles.code}>{profile.connectCode}</div>}
          <div className={styles.standing} data-testid="profile-standing">
            {standing}
          </div>
        </div>
      </div>

      <div className={styles.section}>
        <div className={styles.sectionTitle}>{Messages.ranked()}</div>
        <dl className={styles.stats}>
          {(r.rating != null || r.setsPlayed > 0) && (
            <Stat label={Messages.rating()} value={r.rating != null ? formatRating(r.rating) : HIDDEN_RATING} />
          )}
          {position && <Stat label={Messages.leaderboard()} value={position} />}
          <Stat label={Messages.setsPlayed()} value={String(r.setsPlayed)} />
          {r.setsPlayed > 0 && <Stat label={Messages.record()} value={Messages.recordValue(r.wins, r.losses)} />}
        </dl>
      </div>

      {(actions || profile.link) && (
        <div className={styles.footer}>
          <div className={styles.actions}>{actions}</div>
          {profile.link && <WebLink link={profile.link} onCopy={onCopyLink} />}
        </div>
      )}
    </div>
  );
});

const Stat = ({ label, value }: { label: string; value: string }) => (
  <div>
    <dt>{label}</dt>
    <dd>{value}</dd>
  </div>
);

/** The profile's web page: its address (opens in the browser) and a copy button. */
const WebLink = ({ link, onCopy }: { link: string; onCopy?: (link: string) => void }) => {
  const shown = link.replace(/^https?:\/\//, "");
  return (
    <div className={styles.webLink}>
      <Tooltip title={Messages.openInBrowser()}>
        <a
          href={link}
          onClick={(e) => {
            e.preventDefault();
            window.electron.shell.openExternal(link).catch(() => undefined);
          }}
        >
          {shown}
          <OpenInNewIcon sx={{ fontSize: 13, marginLeft: "4px", verticalAlign: "-2px" }} />
        </a>
      </Tooltip>
      {onCopy && (
        <Tooltip title={Messages.copyLink()}>
          <IconButton size="small" aria-label={Messages.copyLink()} onClick={() => onCopy(link)}>
            <ContentCopyIcon sx={{ fontSize: 15 }} />
          </IconButton>
        </Tooltip>
      )}
    </div>
  );
};
