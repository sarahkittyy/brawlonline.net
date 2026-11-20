import { DISCORD_INVITE_URL, PRODUCT_NAME } from "@common/product";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import KeyboardDoubleArrowLeftRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowLeftRounded";
import KeyboardDoubleArrowRightRoundedIcon from "@mui/icons-material/KeyboardDoubleArrowRightRounded";
import LoginRoundedIcon from "@mui/icons-material/LoginRounded";
import MenuRoundedIcon from "@mui/icons-material/MenuRounded";
import NotificationsOffOutlinedIcon from "@mui/icons-material/NotificationsOffOutlined";
import NotificationsOutlinedIcon from "@mui/icons-material/NotificationsOutlined";
import SettingsOutlinedIcon from "@mui/icons-material/SettingsOutlined";
import Button from "@mui/material/Button";
import ButtonBase from "@mui/material/ButtonBase";
import IconButton from "@mui/material/IconButton";
import Popover from "@mui/material/Popover";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import log from "electron-log";
import debounce from "lodash/debounce";
import React, { useLayoutEffect, useMemo, useRef, useState } from "react";
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
import { WindowDragStrip } from "./window_drag_strip";

const isMac = window.electron.bootstrap.isMac;

/** Open, or folded to its icons; remembered on this computer. */
type SidebarChoice = "open" | "folded";
const SIDEBAR_KEY = "SIDEBAR";

/** The spacer's least height between the pages and the bottom group (sidebar.module.css .spacer). */
const SPACER_MIN = 12;
/** The folded tools as a column (6 px over five 30 px buttons) less as the menu button (6 px over one). */
const TOOLS_SAVED = 4 * 30;

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
 * tooltips, the player's stock icon in the card's box, and the tools one under another. In a window too short for
 * that column the tools are one menu button whose popup rolls up over everything with the five in it; the bottom
 * group (the arrows, the search, the card, the tools) always shows and only the pages' icons scroll, faded where they
 * are cut off (Sarah's board: "at smaller vertical heights, replace it with a menu button that rolls up a vertical pop
 * up over everything that has the 5 buttons in it").
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
  const fit = useFoldedFit(folded);

  return (
    <aside
      ref={fit.asideRef}
      className={clsx(styles.side, folded && styles.folded, isMac && styles.mac)}
      data-testid="sidebar"
      data-folded={folded ? "" : undefined}
    >
      <div ref={fit.topRef} className={styles.top} data-cut={fit.cut ? "" : undefined}>
        <div ref={fit.innerRef} className={styles.group}>
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
        </div>
      </div>
      <div className={styles.spacer} />
      <div ref={fit.bottomRef} className={styles.group}>
        {/* The fold arrows: at the bottom, over the search and the player's card (Sarah's board). */}
        <div className={styles.foldRow}>
          {/* A new tooltip on each fold: the button moves out from under the cursor, so the old one never heard the
              mouse leave and stayed on the screen with the new label (Sarah's board). */}
          <Tooltip
            key={folded ? "folded" : "open"}
            title={foldLabel}
            placement={folded ? "right" : "top"}
            disableInteractive={true}
          >
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
        {fit.menu ? (
          <ToolsMenu openSettings={() => openSettings()} />
        ) : (
          <div className={styles.tools}>
            <Tools placement={toolTip} openSettings={() => openSettings()} />
          </div>
        )}
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
      {/* The strip by the window buttons (sidebar.module.css .mac). */}
      <WindowDragStrip height={34} />
    </aside>
  );
};

/**
 * Folded: whether the tools must be a menu (the page icons, the bottom group with the tools' column and the spacer
 * don't fit the sidebar's height) and whether the page icons are cut off at the bottom. The column comes back once
 * there is room for it again, so a window at the edge doesn't flip between the two.
 */
const useFoldedFit = (folded: boolean) => {
  const asideRef = useRef<HTMLElement>(null);
  const topRef = useRef<HTMLDivElement>(null);
  const innerRef = useRef<HTMLDivElement>(null);
  const bottomRef = useRef<HTMLDivElement>(null);
  const [menu, setMenu] = useState(false);
  const [cut, setCut] = useState(false);
  const menuNow = useRef(menu);
  menuNow.current = menu;

  useLayoutEffect(() => {
    const aside = asideRef.current;
    const top = topRef.current;
    const inner = innerRef.current;
    const bottom = bottomRef.current;
    if (!folded || !aside || !top || !inner || !bottom || typeof ResizeObserver === "undefined") {
      setMenu(false);
      setCut(false);
      return;
    }
    const check = () => {
      const style = getComputedStyle(aside);
      const room = aside.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom) - SPACER_MIN;
      const withColumn = inner.offsetHeight + bottom.offsetHeight + (menuNow.current ? TOOLS_SAVED : 0);
      setMenu(withColumn > room + 0.5);
      setCut(top.scrollTop + top.clientHeight < top.scrollHeight - 1);
    };
    check();
    const observer = new ResizeObserver(check);
    for (const el of [aside, top, inner, bottom]) {
      observer.observe(el);
    }
    top.addEventListener("scroll", check, { passive: true });
    return () => {
      observer.disconnect();
      top.removeEventListener("scroll", check);
    };
  }, [folded]);

  return { asideRef, topRef, innerRef, bottomRef, menu, cut };
};

/**
 * Settings, Theme Color, Report a bug, Discord and the bell; in a column (folded, or the folded menu's popup) the other
 * way up, the bell on top and Settings at the bottom (Sarah's board: "flip the order. settings on the bottom, notifs on
 * top").
 */
const Tools = ({ placement, openSettings }: { placement?: "right"; openSettings: () => void }) => {
  const tools = [
    <Tooltip key="settings" title={HeaderMessages.settings()} placement={placement}>
      <IconButton aria-label={HeaderMessages.settings()} onClick={openSettings}>
        <SettingsOutlinedIcon />
      </IconButton>
    </Tooltip>,
    <ThemeColorButton key="theme" placement={placement} />,
    <BugReportButton key="bug" placement={placement} />,
    <Tooltip key="discord" title={HeaderMessages.discord()} placement={placement}>
      <IconButton
        component="a"
        href={DISCORD_INVITE_URL}
        target="_blank"
        rel="noopener noreferrer"
        aria-label={HeaderMessages.discord()}
      >
        <DiscordIcon />
      </IconButton>
    </Tooltip>,
    <SoundsToggle key="sounds" placement={placement} />,
  ];
  // Beside the column (tooltips at the right) = a column.
  return <>{placement === "right" ? tools.reverse() : tools}</>;
};

/**
 * The folded tools in a short window: a menu button whose popup rolls up over everything with the five in a column.
 * A pick closes it; the popup stays mounted so Report a bug's dialog (inside its button) stays open.
 */
const ToolsMenu = ({ openSettings }: { openSettings: () => void }) => {
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const close = () => setAnchor(null);
  return (
    <div className={styles.tools}>
      <Tooltip title={anchor ? "" : Messages.toolsMenu()} placement="right" disableInteractive={true}>
        <IconButton
          aria-label={Messages.toolsMenu()}
          aria-haspopup="true"
          aria-expanded={anchor != null}
          data-testid="sidebar-tools-menu"
          onClick={(event) => setAnchor(event.currentTarget)}
        >
          <MenuRoundedIcon />
        </IconButton>
      </Tooltip>
      <Popover
        open={anchor != null}
        anchorEl={anchor}
        onClose={close}
        keepMounted={true}
        // Centred on the button: MUI's default 16 px from the window's edge pushed it 6 px right of the column.
        marginThreshold={4}
        anchorOrigin={{ vertical: "top", horizontal: "center" }}
        transformOrigin={{ vertical: "bottom", horizontal: "center" }}
        slotProps={{ paper: { className: styles.toolsPopup } }}
      >
        <div className={styles.toolsColumn} onClick={close} data-testid="sidebar-tools-popup">
          <Tools placement="right" openSettings={openSettings} />
        </div>
      </Popover>
    </div>
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
