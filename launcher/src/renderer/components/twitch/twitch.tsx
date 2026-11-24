import type { TwitchStream } from "@accounts/types";
import VisibilityOutlinedIcon from "@mui/icons-material/VisibilityOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";

import { CharacterIcon } from "@/components/profile/character_icon";
import { SOCIAL_ICON_PATHS } from "@/components/profile/social_icons";
import { openTwitch, useTwitchActions, useTwitchLink } from "@/lib/twitch/use_twitch";

import { TwitchMessages as Messages } from "./twitch.messages";
import styles from "./twitch.module.css";

/**
 * Twitch around the launcher (docs/design/twitch-live.md). Its logo is always Twitch's purple on nothing (Sarah's board:
 * "you'd just use the purple fg / transparent bg big icon for both").
 */
export const TwitchGlyph = ({ size = 14, className }: { size?: number; className?: string }) => (
  <svg
    viewBox="0 0 24 24"
    width={size}
    height={size}
    className={clsx(styles.glyph, className)}
    aria-hidden={true}
    focusable={false}
  >
    <path d={SOCIAL_ICON_PATHS.twitch} />
  </svg>
);

/**
 * A live player's mark beside their name (the online list, by the flag; Spectate's players): the logo, which opens the
 * stream. `corner`: over the corner of a picture instead (the folded players rail), taking no room.
 */
export const TwitchLiveMark = ({
  stream,
  size = 14,
  corner = false,
  className,
}: {
  /** Nothing without one (the player isn't live). */
  stream: TwitchStream | null | undefined;
  size?: number;
  corner?: boolean;
  className?: string;
}) =>
  stream ? (
    <Tooltip title={Messages.account(stream.login)} disableInteractive={true}>
      <button
        type="button"
        className={clsx(styles.mark, corner && styles.corner, className)}
        aria-label={Messages.account(stream.login)}
        data-testid="twitch-live"
        onClick={(e) => {
          e.stopPropagation();
          openTwitch(stream.login);
        }}
        onKeyDown={(e) => e.stopPropagation()}
      >
        <TwitchGlyph size={size} />
      </button>
    </Tooltip>
  ) : null;

/** The red live dot, the eye and the viewers, over a stream's picture. */
const Viewers = ({ viewers }: { viewers: number }) => (
  <span className={styles.viewers}>
    <span className={styles.liveDot} />
    <VisibilityOutlinedIcon style={{ fontSize: 13 }} />
    {viewers}
  </span>
);

/**
 * A player's stream in their hover card (`mini`) and their profile (`big`): the picture, the Twitch name, the title and
 * Twitch's category. The profile's opens the stream; the hover card can't be clicked.
 */
export const TwitchStreamPanel = ({ stream, size }: { stream: TwitchStream; size: "mini" | "big" }) => {
  const body = (
    <>
      <span className={styles.panelPicture}>
        <img src={stream.thumbnail} alt="" draggable={false} />
        <Viewers viewers={stream.viewers} />
      </span>
      <span className={styles.panelText}>
        <span className={styles.panelName}>
          <TwitchGlyph size={size === "big" ? 15 : 13} />
          {stream.twitchName}
        </span>
        <span className={styles.panelTitle}>{stream.title}</span>
        {stream.game && <span className={styles.panelGame}>{stream.game}</span>}
      </span>
    </>
  );
  if (size === "mini") {
    return (
      <div className={clsx(styles.panel, styles.mini)} data-testid="twitch-panel">
        {body}
      </div>
    );
  }
  return (
    <button
      type="button"
      className={clsx(styles.panel, styles.big)}
      onClick={() => openTwitch(stream.login)}
      aria-label={Messages.account(stream.login)}
      data-testid="twitch-panel"
    >
      {body}
    </button>
  );
};

/** Spectate's Twitch row: one stream, its picture over the player and the title. Opens the stream. */
export const TwitchStreamCard = ({ stream }: { stream: TwitchStream }) => (
  <button
    type="button"
    className={styles.card}
    onClick={() => openTwitch(stream.login)}
    title={Messages.account(stream.login)}
    data-testid="twitch-card"
  >
    <span className={styles.cardPicture}>
      <img src={stream.thumbnail} alt="" draggable={false} />
      <Viewers viewers={stream.viewers} />
      <span className={styles.cardLogo}>
        <svg viewBox="0 0 24 24" width={12} height={12} aria-hidden={true} focusable={false}>
          <path d={SOCIAL_ICON_PATHS.twitch} />
        </svg>
      </span>
    </span>
    <span className={styles.cardName}>
      {stream.main && <CharacterIcon character={stream.main} size={16} />}
      <span className={styles.cardNameText}>{stream.displayName}</span>
    </span>
    <span className={styles.cardTitle}>{stream.title}</span>
  </button>
);

/**
 * Connect account: opens Twitch's sign-in in the browser; spins until the account is connected (or the wait runs out).
 * Nothing while the server can't connect Twitch accounts. `glow`: the profile hint's last step (use_twitch.ts).
 */
export const TwitchConnectButton = ({
  glow = false,
  variant = "outlined",
  className,
}: {
  glow?: boolean;
  variant?: "outlined" | "contained" | "text";
  className?: string;
}) => {
  const link = useTwitchLink();
  const { connect, waiting } = useTwitchActions();
  if (!link?.available) {
    return null;
  }
  return (
    <Button
      size="small"
      variant={variant}
      color="secondary"
      className={clsx(styles.connect, glow && styles.glow, className)}
      onClick={() => void connect()}
      disabled={waiting}
      startIcon={waiting ? <CircularProgress size={12} color="inherit" /> : <TwitchGlyph size={14} />}
      data-testid="twitch-connect"
    >
      {Messages.connectAccount()}
    </Button>
  );
};

/**
 * The player's own Twitch connection, for Settings > Social and the profile's edit mode: Connect account, or the
 * connected account (its page) and Disconnect.
 */
export const TwitchConnection = ({ glow = false }: { glow?: boolean }) => {
  const link = useTwitchLink();
  const { disconnect } = useTwitchActions();
  if (!link?.available) {
    return null;
  }
  if (!link.linked) {
    return <TwitchConnectButton glow={glow} />;
  }
  const login = link.linked.login;
  return (
    <span className={styles.connected} data-testid="twitch-connected">
      <button type="button" className={styles.connectedName} onClick={() => openTwitch(login)}>
        <TwitchGlyph size={14} />
        {link.linked.displayName || login}
      </button>
      <Button size="small" className={styles.disconnect} onClick={() => void disconnect()}>
        {Messages.disconnect()}
      </Button>
    </span>
  );
};
