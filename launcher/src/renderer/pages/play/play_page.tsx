import { GAME_NAME } from "@common/product";
import CachedIcon from "@mui/icons-material/Cached";
import CloseIcon from "@mui/icons-material/Close";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import { useMutation } from "@tanstack/react-query";
import React from "react";

import { PlayButtonMessages } from "@/app/header/play_button/play_button.messages";
import { ProfileMessages } from "@/components/profile/profile.messages";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { Sparkline } from "@/components/sparkline/sparkline";
import { usePlay } from "@/lib/dolphin/use_play";
import { useAccount } from "@/lib/hooks/use_account";
import { useRatingHistory } from "@/lib/hooks/use_rating_history";
import { useEnableRankDisplay, useLaunchGameOnPlay } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { formatPosition, formatProfileRating } from "@/lib/ranked_format";
import { useRoomList } from "@/lib/social/use_room_list";
import { PublicRooms } from "@/pages/home/overview/public_rooms/public_rooms";
import { PublicRoomsMessages } from "@/pages/home/overview/public_rooms/public_rooms.messages";
import { PlayButtons } from "@/pages/home/play_buttons/play_buttons";
import { ProjectPunchButton } from "@/pages/home/project_punch_button/project_punch_button";
import { GameSettingsMessages } from "@/pages/settings/game_settings/game_settings.messages";
import { useServices } from "@/services";
import type { RankedProfile } from "@/services/backend/types";

import { PlayPageMessages as MyRankingMessages } from "./play_page.messages";
import styles from "./play_page.module.css";

/**
 * Play (docs/design/launcher-redesign.md §2, §8), the launcher's first page: the players in game; one panel with the
 * player's ranked card on the left and Play unranked / ranked / 2v2 stacked on the right; then the public rooms. The
 * game itself starts from the button at the top right, as the old header's Play did (Settings > Play Button Action).
 */
export const PlayPage = React.memo(function PlayPage() {
  const { query } = useRoomList();
  const online = query.data?.online;
  return (
    <div className={styles.page}>
      <div className={styles.head}>
        <h1 className={styles.title}>{PlayButtonMessages.play()}</h1>
        {online != null && (
          <span className={styles.inGame} data-testid="players-online">
            <span className={styles.dot} />
            {PublicRoomsMessages.playersInGame(online)}
          </span>
        )}
        <span className={styles.grow} />
        <LaunchButton />
        <ProjectPunchButton />
      </div>
      <section className={styles.panel}>
        <div className={styles.rankedSide}>
          <RankedCard />
        </div>
        <PlayButtons />
      </section>
      <PublicRooms />
    </div>
  );
});

/** Starts the game (or Dolphin), as the old header's Play button did. */
const LaunchButton = () => {
  const play = usePlay();
  const [launchGame] = useLaunchGameOnPlay();
  return (
    <Button
      variant="outlined"
      color="inherit"
      className={styles.launch}
      startIcon={<PlayArrowRoundedIcon />}
      onClick={() => void play({})}
      data-testid="launch-game"
    >
      {launchGame ? GameSettingsMessages.launchMelee(GAME_NAME) : GameSettingsMessages.launchDolphin()}
    </Button>
  );
};

/** The player's rank, rating, position and record, and the rating's last sets as a line. */
const RankedCard = () => {
  const [enabled, setEnabled] = useEnableRankDisplay();
  const { showInfo } = useToasts();
  const profile = useAccount((s) => s.userData?.rankedNetplayProfile);
  const history = useRatingHistory();
  if (!enabled || !profile) {
    return null;
  }
  const position = formatPosition(profile.position, profile.rankedPlayers);
  const record =
    profile.wins != null && profile.losses != null ? ProfileMessages.recordValue(profile.wins, profile.losses) : null;
  const sub = [MyRankingMessages.setsPlayed(profile.setsPlayed), position, record].filter(Boolean).join(" · ");
  return (
    <div className={styles.ranked} data-testid="ranked-card" data-tier={profile.rank?.tier ?? "none"}>
      <span className={styles.emblem}>
        <RankBadge rank={profile.rank} setsToPlace={profile.setsToPlace} size="large" />
      </span>
      <div className={styles.numbers}>
        {profile.rank && <div className={styles.tier}>{RankBadgeMessages.rankName(profile.rank)}</div>}
        <div className={styles.rating}>{formatProfileRating(profile)}</div>
        <div className={styles.sub}>{sub}</div>
      </div>
      <Sparkline values={history} className={styles.spark} />
      <div className={styles.cardTools}>
        <RefreshRating />
        <Tooltip title={MyRankingMessages.hide()}>
          <IconButton
            size="small"
            aria-label={MyRankingMessages.hide()}
            onClick={() => void setEnabled(false).then(() => showInfo(MyRankingMessages.rankingHiddenNotification()))}
          >
            <CloseIcon fontSize="small" />
          </IconButton>
        </Tooltip>
      </div>
    </div>
  );
};

/** Reads the rating again (it changes after each set the server rates). */
const RefreshRating = () => {
  const updateRanking = useAccount((s) => s.updateRanking);
  const user = useAccount((s) => s.user);
  const { backendService } = useServices();
  const mutation = useMutation({
    mutationFn: async (uid: string) => {
      const profile: RankedProfile | undefined = await backendService.fetchRankedNetplayProfile(uid);
      if (profile && user?.uid === uid) {
        updateRanking(profile);
      }
      return profile;
    },
  });
  return (
    <Tooltip title={MyRankingMessages.refresh()}>
      <span>
        <IconButton
          size="small"
          aria-label={MyRankingMessages.refresh()}
          disabled={mutation.isPending || !user}
          onClick={() => user && mutation.mutate(user.uid)}
        >
          {mutation.isPending ? <CircularProgress color="inherit" size={16} /> : <CachedIcon fontSize="small" />}
        </IconButton>
      </span>
    </Tooltip>
  );
};
