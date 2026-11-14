import KeyboardDoubleArrowLeftRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowLeftRounded";
import KeyboardDoubleArrowRightRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowRightRounded";
import MailOutlineRoundedIcon from "@mui/icons-material/MailOutlineRounded";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import useMediaQuery from "@mui/material/useMediaQuery";
import type { SocialPlayer } from "@social/types";
import React, { useMemo } from "react";

import { MainPicture } from "@/components/main_picture/main_picture";
import { useStockIcon } from "@/components/profile/character_icon";
import { FriendsIcon } from "@/components/social/friend_icons";
import { SocialMessages } from "@/components/social/social.messages";
import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useFriendUids } from "@/lib/social/use_friends";
import { useSocial } from "@/lib/social/use_social";
import { OnlinePlayers } from "@/pages/home/overview/online_players/online_players";

import styles from "./players_rail.module.css";
import { ShellMessages as Messages } from "./shell.messages";

/** The player's choice, remembered on this computer: `auto` folds the rail in a narrow window. */
type RailChoice = "auto" | "open" | "folded";
const RAIL_KEY = "PLAYERS_RAIL";

/** Narrower than this, the rail folds by itself (until the player opens it). */
const NARROW = "(max-width: 1149px)";

/**
 * The right column on every page (docs/design/launcher-redesign.md §2): the Online / Friends panel that was on Home.
 * It folds to a strip of the friends online (a click opens a chat with one) and the unread envelope.
 */
export const PlayersRail = React.memo(function PlayersRail() {
  const [choice, setChoice] = useLocalStorage<RailChoice>(RAIL_KEY, "auto");
  const narrow = useMediaQuery(NARROW);
  const folded = choice === "folded" || (choice === "auto" && narrow);
  if (folded) {
    return <FoldedRail onOpen={() => setChoice("open")} />;
  }
  return (
    <aside className={styles.rail} data-testid="players-rail">
      <OnlinePlayers
        className={styles.block}
        trailing={
          <Tooltip title={Messages.hidePlayers()} placement="top" disableInteractive={true}>
            <IconButton
              size="small"
              className={styles.foldButton}
              aria-label={Messages.hidePlayers()}
              data-testid="rail-fold"
              onClick={() => setChoice("folded")}
            >
              <KeyboardDoubleArrowRightRoundedIcon fontSize="small" />
            </IconButton>
          </Tooltip>
        }
      />
    </aside>
  );
});

const FoldedRail = ({ onOpen }: { onOpen: () => void }) => {
  const online = useSocial((s) => s.state.online);
  const conversations = useSocial((s) => s.state.conversations);
  const openTab = useSocial((s) => s.openTab);
  const friendUids = useFriendUids();
  const friendsOnline = useMemo(() => online.filter((p) => friendUids.has(p.uid)), [online, friendUids]);
  const unread = Object.values(conversations).reduce((n, c) => n + c.unread, 0);
  return (
    <aside className={styles.folded} data-testid="players-rail-folded">
      <Tooltip title={Messages.showPlayers()} placement="left">
        <IconButton className={styles.foldButton} aria-label={Messages.showPlayers()} onClick={onOpen}>
          <KeyboardDoubleArrowLeftRoundedIcon />
        </IconButton>
      </Tooltip>
      <Tooltip title={SocialMessages.friends()} placement="left">
        <IconButton className={styles.foldButton} aria-label={SocialMessages.friends()} onClick={onOpen}>
          <FriendsIcon />
        </IconButton>
      </Tooltip>
      {friendsOnline.map((p) => (
        <FriendPicture key={p.uid} player={p} onClick={() => openTab(p.uid, true)} />
      ))}
      <span className={styles.rule} />
      <Tooltip title={SocialMessages.message()} placement="left">
        <IconButton className={styles.foldButton} aria-label={SocialMessages.message()} onClick={onOpen}>
          <MailOutlineRoundedIcon />
          {unread > 0 && <span className={styles.unread}>{unread}</span>}
        </IconButton>
      </Tooltip>
    </aside>
  );
};

const FriendPicture = ({ player, onClick }: { player: SocialPlayer; onClick: () => void }) => {
  const url = useStockIcon(player.main ?? "");
  return (
    <Tooltip title={player.displayName} placement="left">
      <button type="button" className={styles.friend} onClick={onClick} aria-label={player.displayName}>
        <MainPicture url={player.main ? url ?? null : null} size={30} />
        <span className={styles.onlineDot} />
      </button>
    </Tooltip>
  );
};
