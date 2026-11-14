import { DISCORD_INVITE_URL, PRODUCT_NAME } from "@common/product";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import NotificationsOffOutlinedIcon from "@mui/icons-material/NotificationsOffOutlined";
import NotificationsOutlinedIcon from "@mui/icons-material/NotificationsOutlined";
import SettingsOutlinedIcon from "@mui/icons-material/SettingsOutlined";
import Button from "@mui/material/Button";
import ButtonBase from "@mui/material/ButtonBase";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import log from "electron-log";
import debounce from "lodash/debounce";
import React, { useMemo } from "react";
import { Link, useMatch, useResolvedPath } from "react-router-dom";

import { ActivateOnlineDialog } from "@/app/header/activate_online_dialog";
import { BugReportButton } from "@/app/header/bug_report/bug_report_button";
import { HeaderMessages } from "@/app/header/header.messages";
import { LauncherUpdate, useLauncherUpdateVersion } from "@/app/header/play_button/launcher_update";
import { UpdatingButton } from "@/app/header/play_button/play_button";
import { StartGameOfflineDialog } from "@/app/header/start_game_offline_dialog";
import { UserMenu } from "@/app/header/user_menu/user_menu";
import { DiscordIcon } from "@/components/discord_icon";
import { ThemeColorButton } from "@/components/theme_color_picker/theme_color_button";
import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { usePlay, usePlayDialogs } from "@/lib/dolphin/use_play";
import { useAccount } from "@/lib/hooks/use_account";
import { useAppUpdate } from "@/lib/hooks/use_app_update";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useRouteMemory } from "@/lib/hooks/use_route_memory";
import { useNotificationSounds } from "@/lib/hooks/use_settings";
import { useSettingsModal } from "@/lib/hooks/use_settings_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useTournamentsUrl } from "@/lib/hooks/use_tournaments_url";
import { useServices } from "@/services";

import { QueueBox } from "./queue_box";
import styles from "./sidebar.module.css";

const isMac = window.electron.bootstrap.isMac;

export type NavItem = {
  subpath: string;
  title: () => string;
  Icon: React.ComponentType<{ className?: string }>;
  /** A count after the label (Other Games' public rooms, Spectate's live games); null hides it. */
  Badge?: React.ComponentType;
};

/**
 * The launcher's left column (docs/design/launcher-redesign.md §2), in place of Slippi's header: the product name
 * (it checks for updates, as Slippi's logo does), a set-up step or a launcher update while there is one (the old
 * Play button's states), the pages with their names, the tournaments trophy while the server names a page, the
 * player's search or match (QueueBox), the player's card (their own profile) and the tools: Settings, Theme Color,
 * Report a bug, Discord and the notification sounds.
 */
export const Sidebar = ({ items }: { items: readonly NavItem[] }) => {
  const openLogin = useLoginModal((store) => store.openModal);
  const { open: openSettings } = useSettingsModal();
  const currentUser = useAccount((store) => store.user);
  const { showError } = useToasts();
  const play = usePlay();
  const onPlay = (offlineOnly?: boolean) => void play({ offlineOnly });
  const { startGameOpen, activateOnlineOpen, setStartGameOpen, setActivateOnlineOpen } = usePlayDialogs();
  const tournamentsUrl = useTournamentsUrl();

  return (
    <aside className={clsx(styles.side, isMac && styles.mac)} data-testid="sidebar">
      <LogoButton />
      <SetupStatus onClick={() => onPlay()} />
      <nav className={styles.nav}>
        {items.map((item) => (
          <NavLink key={item.subpath} item={item} />
        ))}
        {tournamentsUrl && (
          <a
            className={clsx(styles.navItem, styles.trophy)}
            href={tournamentsUrl}
            target="_blank"
            rel="noopener noreferrer"
          >
            <EmojiEventsRoundedIcon className={styles.navIcon} />
            <span className={styles.navLabel}>{HeaderMessages.tournaments()}</span>
          </a>
        )}
      </nav>
      <div className={styles.spacer} />
      <QueueBox />
      {currentUser ? (
        <UserMenu user={currentUser} handleError={showError} variant="sidebar" />
      ) : (
        <Button className={styles.logIn} variant="contained" onClick={openLogin}>
          {HeaderMessages.logIn()}
        </Button>
      )}
      <div className={styles.tools}>
        <Tooltip title={HeaderMessages.settings()}>
          <IconButton aria-label={HeaderMessages.settings()} onClick={() => openSettings()}>
            <SettingsOutlinedIcon />
          </IconButton>
        </Tooltip>
        <ThemeColorButton />
        <BugReportButton />
        <Tooltip title={HeaderMessages.discord()}>
          <IconButton
            component="a"
            href={DISCORD_INVITE_URL}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={HeaderMessages.discord()}
          >
            <DiscordIcon />
          </IconButton>
        </Tooltip>
        <SoundsToggle />
      </div>
      <StartGameOfflineDialog
        open={startGameOpen}
        onCancel={() => setStartGameOpen(false)}
        onPlayOffline={() => onPlay(true)}
      />
      <ActivateOnlineDialog
        open={activateOnlineOpen}
        onClose={() => setActivateOnlineOpen(false)}
        onSubmit={() => onPlay()}
      />
    </aside>
  );
};

const NavLink = ({ item }: { item: NavItem }) => {
  const resolved = useResolvedPath(item.subpath);
  const match = useMatch({ path: resolved.pathname, end: false });
  const clearLastRoute = useRouteMemory((s) => s.clearLastRoute);
  const { Icon, Badge } = item;
  // Clicking the page you're on opens its first tab (Replays > Highlights -> Replays); coming back from another page
  // still opens the tab you left it on.
  const onClick = () => {
    if (match != null) {
      clearLastRoute(item.subpath);
    }
  };
  return (
    <Link
      to={item.subpath}
      onClick={onClick}
      className={clsx(styles.navItem, match != null && styles.on)}
      aria-current={match != null ? "page" : undefined}
      data-testid={`nav-${item.subpath}`}
    >
      <Icon className={styles.navIcon} />
      <span className={styles.navLabel}>{item.title()}</span>
      {Badge && <Badge />}
    </Link>
  );
};

/** A count after a page's name. */
export const NavBadge = ({ children, live = false }: { children: React.ReactNode; live?: boolean }) => (
  <span className={styles.badge}>
    {live && <span className={styles.liveDot} />}
    {children}
  </span>
);

/** The old Play button while it had something to say: a set-up step's progress, or a launcher update to install. */
const SetupStatus = ({ onClick }: { onClick: () => void }) => {
  const installStatus = useDolphinStore((store) => store.netplayStatus);
  const installProgress = useDolphinStore((store) => store.netplayDownloadProgress);
  const launcherUpdateVersion = useLauncherUpdateVersion();
  const fillPercent = useMemo(() => {
    if (installProgress && installProgress.total > 0) {
      return installProgress.current / installProgress.total;
    }
    return 0;
  }, [installProgress]);
  const settingUp = installStatus === DolphinStatus.DOWNLOADING && installProgress?.phase !== undefined;
  if (launcherUpdateVersion && !settingUp) {
    return (
      <div className={styles.status}>
        <LauncherUpdate />
      </div>
    );
  }
  if (installStatus === DolphinStatus.READY) {
    return null;
  }
  return (
    <div className={styles.status}>
      <UpdatingButton onClick={onClick} fillPercent={fillPercent} phase={installProgress?.phase} />
    </div>
  );
};

/** The product name: it checks for launcher and Dolphin updates, as Slippi's logo does. */
const LogoButton = () => {
  const { dolphinService } = useServices();
  const { updateDolphin } = useDolphinActions(dolphinService);
  const { checkForAppUpdates } = useAppUpdate();
  const { showInfo, showError } = useToasts();
  const [checking, setChecking] = React.useState(false);

  const check = useMemo(() => {
    const run = async () => {
      setChecking(true);
      try {
        const updateResult = await checkForAppUpdates();
        if (updateResult && !updateResult.updateAvailable) {
          showInfo(HeaderMessages.noUpdateAvailable());
        }
        await updateDolphin();
      } catch (err) {
        log.error(err);
        showError(HeaderMessages.failedToGetUpdates());
      } finally {
        setChecking(false);
      }
    };
    return debounce(run, 500);
  }, [checkForAppUpdates, updateDolphin, showInfo, showError]);

  return (
    <div className={styles.logoRow}>
      <Tooltip title={checking ? HeaderMessages.checkingForUpdates() : HeaderMessages.checkForUpdates()}>
        <ButtonBase className={styles.logo} onClick={check} disabled={checking}>
          {PRODUCT_NAME}
        </ButtonBase>
      </Tooltip>
    </div>
  );
};

/** The bell: chat message and invite sounds on or off. */
const SoundsToggle = () => {
  const [on, setOn] = useNotificationSounds();
  return (
    <Tooltip title={HeaderMessages.toggleNotificationSounds()}>
      <IconButton
        onClick={() => void setOn(!on)}
        aria-pressed={on}
        aria-label={HeaderMessages.toggleNotificationSounds()}
      >
        {on ? <NotificationsOutlinedIcon /> : <NotificationsOffOutlinedIcon />}
      </IconButton>
    </Tooltip>
  );
};
