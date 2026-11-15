import { DISCORD_INVITE_URL, PRODUCT_NAME } from "@common/product";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import KeyboardDoubleArrowLeftRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowLeftRounded";
import KeyboardDoubleArrowRightRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowRightRounded";
import LoginRoundedIcon from "@mui/icons-material/LoginRounded";
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
import { useLauncherUpdate } from "@/app/header/play_button/launcher_update";
import { LauncherUpdateButton, UpdatingButton } from "@/app/header/play_button/play_button";
import { StartGameOfflineDialog } from "@/app/header/start_game_offline_dialog";
import { UserMenu } from "@/app/header/user_menu/user_menu";
import { DiscordIcon } from "@/components/discord_icon";
import { ThemeColorButton } from "@/components/theme_color_picker/theme_color_button";
import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { usePlay, usePlayDialogs } from "@/lib/dolphin/use_play";
import { useAccount } from "@/lib/hooks/use_account";
import { useAppUpdate } from "@/lib/hooks/use_app_update";
import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useRouteMemory } from "@/lib/hooks/use_route_memory";
import { useNotificationSounds } from "@/lib/hooks/use_settings";
import { useSettingsModal } from "@/lib/hooks/use_settings_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useTournamentsUrl } from "@/lib/hooks/use_tournaments_url";
import { useServices } from "@/services";

import logo from "./images/logo.png";
import { QueueBox } from "./queue_box";
import { ShellMessages as Messages } from "./shell.messages";
import styles from "./sidebar.module.css";
import { LauncherUpdateRing, SetupRing } from "./update_ring";

const isMac = window.electron.bootstrap.isMac;

/** Open, or folded to its icons; remembered on this computer. */
type SidebarChoice = "open" | "folded";
const SIDEBAR_KEY = "SIDEBAR";

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
 *
 * It folds as the players rail does, to a column as wide as the folded rail (§14): the logo (the app icon's ball; it
 * still checks for updates), a ring for a set-up step or a launcher update, the pages' icons with their names in
 * tooltips, the player's stock icon in the card's box, and the tools one under another.
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
  const [choice, setChoice] = useLocalStorage<SidebarChoice>(SIDEBAR_KEY, "open");
  const folded = choice === "folded";
  // Folded, the tools are a column: their names show beside them, not over the next one.
  const toolTip = folded ? "right" : undefined;
  const foldLabel = folded ? Messages.openSidebar() : Messages.foldSidebar();

  return (
    <aside
      className={clsx(styles.side, folded && styles.folded, isMac && styles.mac)}
      data-testid="sidebar"
      data-folded={folded ? "" : undefined}
    >
      <div className={styles.logoRow}>
        <LogoButton folded={folded} />
      </div>
      <SetupStatus onClick={() => onPlay()} folded={folded} />
      <nav className={styles.nav}>
        {items.map((item) => (
          <NavLink key={item.subpath} item={item} folded={folded} />
        ))}
        {tournamentsUrl && (
          <Tooltip title={folded ? HeaderMessages.tournaments() : ""} placement="right" disableInteractive={true}>
            <a
              className={clsx(styles.navItem, styles.trophy)}
              href={tournamentsUrl}
              target="_blank"
              rel="noopener noreferrer"
              aria-label={folded ? HeaderMessages.tournaments() : undefined}
            >
              <EmojiEventsRoundedIcon className={styles.navIcon} />
              {!folded && <span className={styles.navLabel}>{HeaderMessages.tournaments()}</span>}
            </a>
          </Tooltip>
        )}
      </nav>
      <div className={styles.spacer} />
      {/* The fold arrows: at the bottom, over the search and the player's card (Sarah's board). */}
      <div className={styles.foldRow}>
        <Tooltip title={foldLabel} placement={folded ? "right" : "top"} disableInteractive={true}>
          <IconButton
            size="small"
            className={styles.foldButton}
            aria-label={foldLabel}
            aria-expanded={!folded}
            data-testid={folded ? "sidebar-open" : "sidebar-fold"}
            onClick={() => setChoice(folded ? "open" : "folded")}
          >
            {folded ? (
              <KeyboardDoubleArrowRightRoundedIcon fontSize="small" />
            ) : (
              <KeyboardDoubleArrowLeftRoundedIcon fontSize="small" />
            )}
          </IconButton>
        </Tooltip>
      </div>
      <QueueBox folded={folded} />
      {currentUser ? (
        <UserMenu user={currentUser} handleError={showError} variant={folded ? "folded" : "sidebar"} />
      ) : folded ? (
        <Tooltip title={HeaderMessages.logIn()} placement="right" disableInteractive={true}>
          <IconButton className={styles.logInIcon} aria-label={HeaderMessages.logIn()} onClick={openLogin}>
            <LoginRoundedIcon />
          </IconButton>
        </Tooltip>
      ) : (
        <Button className={styles.logIn} variant="contained" onClick={openLogin}>
          {HeaderMessages.logIn()}
        </Button>
      )}
      <div className={styles.tools}>
        <Tooltip title={HeaderMessages.settings()} placement={toolTip}>
          <IconButton aria-label={HeaderMessages.settings()} onClick={() => openSettings()}>
            <SettingsOutlinedIcon />
          </IconButton>
        </Tooltip>
        <ThemeColorButton placement={toolTip} />
        <BugReportButton placement={toolTip} />
        <Tooltip title={HeaderMessages.discord()} placement={toolTip}>
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
        <SoundsToggle placement={toolTip} />
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

const NavLink = ({ item, folded }: { item: NavItem; folded: boolean }) => {
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
  // Folded: the icon alone with its name in a tooltip (as the folded rail's friends), the count on its corner.
  return (
    <Tooltip title={folded ? item.title() : ""} placement="right" disableInteractive={true}>
      <Link
        to={item.subpath}
        onClick={onClick}
        className={clsx(styles.navItem, match != null && styles.on)}
        aria-current={match != null ? "page" : undefined}
        aria-label={folded ? item.title() : undefined}
        data-testid={`nav-${item.subpath}`}
      >
        <Icon className={styles.navIcon} />
        {!folded && <span className={styles.navLabel}>{item.title()}</span>}
        {Badge && <Badge />}
      </Link>
    </Tooltip>
  );
};

/** A count after a page's name. */
export const NavBadge = ({ children, live = false }: { children: React.ReactNode; live?: boolean }) => (
  <span className={styles.badge}>
    {live && <span className={styles.liveDot} />}
    {children}
  </span>
);

/**
 * The old Play button while it had something to say: a set-up step's progress, or a launcher update to install.
 * Folded, a ring that fills as the button does (update_ring.tsx).
 */
const SetupStatus = ({ onClick, folded }: { onClick: () => void; folded: boolean }) => {
  const installStatus = useDolphinStore((store) => store.netplayStatus);
  const installProgress = useDolphinStore((store) => store.netplayDownloadProgress);
  const launcherUpdate = useLauncherUpdate();
  const fillPercent = useMemo(() => {
    if (installProgress && installProgress.total > 0) {
      return installProgress.current / installProgress.total;
    }
    return 0;
  }, [installProgress]);
  const settingUp = installStatus === DolphinStatus.DOWNLOADING && installProgress?.phase !== undefined;
  if (launcherUpdate && !settingUp) {
    return (
      <div className={styles.status}>
        {folded ? <LauncherUpdateRing update={launcherUpdate} /> : <LauncherUpdateButton {...launcherUpdate} />}
      </div>
    );
  }
  if (installStatus === DolphinStatus.READY) {
    return null;
  }
  return (
    <div className={styles.status}>
      {folded ? (
        <SetupRing phase={installProgress?.phase} fillPercent={fillPercent} />
      ) : (
        <UpdatingButton onClick={onClick} fillPercent={fillPercent} phase={installProgress?.phase} />
      )}
    </div>
  );
};

/** The product name (folded: the app icon's ball): it checks for launcher and Dolphin updates, as Slippi's logo does. */
const LogoButton = ({ folded }: { folded: boolean }) => {
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
    <Tooltip
      title={checking ? HeaderMessages.checkingForUpdates() : HeaderMessages.checkForUpdates()}
      placement={folded ? "right" : "bottom"}
    >
      <ButtonBase
        className={clsx(styles.logo, folded && styles.logoIcon)}
        onClick={check}
        disabled={checking}
        aria-label={folded ? PRODUCT_NAME : undefined}
        data-testid="sidebar-logo"
      >
        {folded ? <img src={logo} alt="" width={32} height={32} draggable={false} /> : PRODUCT_NAME}
      </ButtonBase>
    </Tooltip>
  );
};

/** The bell: chat message and invite sounds on or off. */
const SoundsToggle = ({ placement }: { placement?: "right" }) => {
  const [on, setOn] = useNotificationSounds();
  return (
    <Tooltip title={HeaderMessages.toggleNotificationSounds()} placement={placement}>
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
