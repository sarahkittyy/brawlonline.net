import { DISCORD_INVITE_URL, PRODUCT_NAME, TOURNAMENTS_URL } from "@common/product";
import { css, keyframes } from "@emotion/react";
import styled from "@emotion/styled";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import NotificationsOffOutlinedIcon from "@mui/icons-material/NotificationsOffOutlined";
import NotificationsOutlinedIcon from "@mui/icons-material/NotificationsOutlined";
import SettingsOutlinedIcon from "@mui/icons-material/SettingsOutlined";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import log from "electron-log";
import debounce from "lodash/debounce";
import React, { useMemo } from "react";

import { DiscordIcon } from "@/components/discord_icon";
import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { usePlay, usePlayDialogs } from "@/lib/dolphin/use_play";
import { useAccount } from "@/lib/hooks/use_account";
import { useAppUpdate } from "@/lib/hooks/use_app_update";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useNotificationSounds } from "@/lib/hooks/use_settings";
import { useSettingsModal } from "@/lib/hooks/use_settings_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";
import { platformTitleBarStyles } from "@/styles/platform_title_bar_styles";
import { titleFont } from "@/styles/with_font";

import { ActivateOnlineDialog } from "./activate_online_dialog";
import { BugReportButton } from "./bug_report/bug_report_button";
import { HeaderMessages as Messages } from "./header.messages";
import type { MenuItem } from "./main_menu";
import { MainMenu } from "./main_menu";
import { LauncherUpdate, useLauncherUpdateVersion } from "./play_button/launcher_update";
import { PlayButton as PlayButtonImpl, UpdatingButton } from "./play_button/play_button";
import { StartGameOfflineDialog } from "./start_game_offline_dialog";
import { UserMenu } from "./user_menu/user_menu";

const isMac = window.electron.bootstrap.isMac;

/** Narrower than this (a small window, or a large Interface Size), the header's right side is
 * compact: the account's picture without its name (user_info.module.css) and tighter icons. The
 * full header with a 160 px name needs 1248 px (in the game theme). */
const COMPACT_HEADER = "@media (width <= 1250px)";

/** Narrower steps, each fitting the header (logged in) down to the next: compact needs 1003 px,
 * then 904, 848 and 756. The smallest window (main.ts, 980 px) is 771 px wide at 125 % Interface
 * Size. Measured in the game theme, whose title font is wider than the fallback's. */
const NARROW_TABS = "@media (width <= 1010px)";
const NARROWER_HEADER = "@media (width <= 910px)";
const NARROWEST_HEADER = "@media (width <= 850px)";

const OuterBox = styled(Box)`
  background-color: var(--surface-1);
  height: 70px;
`;

export const Header = ({ menuItems }: { menuItems: readonly MenuItem[] }) => {
  const openModal = useLoginModal((store) => store.openModal);
  const { open } = useSettingsModal();
  const currentUser = useAccount((store) => store.user);
  const { showError } = useToasts();
  // The Play checks and launch live in `usePlay`, so a room in Home's list can start the game too.
  const play = usePlay();
  const onPlay = (offlineOnly?: boolean) => void play({ offlineOnly });
  const { startGameOpen, activateOnlineOpen, setStartGameOpen, setActivateOnlineOpen } = usePlayDialogs();

  return (
    <OuterBox
      css={css`
        display: flex;
        justify-content: space-between;
        ${platformTitleBarStyles()}
      `}
    >
      <div
        css={css`
          display: flex;
          align-items: center;
          padding-left: 5px;
        `}
      >
        <CheckForUpdatesButton />
        <div
          css={css`
            margin: 0 10px;
            ${NARROWEST_HEADER} {
              margin: 0 6px;
            }
          `}
        >
          <PlayButton onClick={() => onPlay()} />
        </div>
        <div
          css={css`
            display: flex;
            height: 100%;
            /* Narrower still (a large Interface Size): the tab icons give up some of their room. */
            ${NARROW_TABS} {
              & .MuiButton-root {
                min-width: 44px;
                padding: 6px 10px;
              }
            }
            ${NARROWER_HEADER} {
              & .MuiButton-root {
                min-width: 40px;
                padding: 6px 8px;
              }
            }
            ${NARROWEST_HEADER} {
              & .MuiButton-root {
                min-width: 32px;
                padding: 6px 4px;
              }
            }
          `}
        >
          <MainMenu menuItems={menuItems} />
          <TournamentsButton />
        </div>
      </div>
      <Box
        display="flex"
        alignItems="center"
        css={css`
          flex-shrink: 0;
          ${COMPACT_HEADER} {
            & .MuiIconButton-root {
              padding: 8px;
            }
          }
          ${NARROWER_HEADER} {
            & .MuiIconButton-root {
              padding: 4px;
            }
          }
        `}
      >
        {currentUser ? (
          <UserMenu user={currentUser} handleError={showError} />
        ) : (
          <Button onClick={openModal} sx={{ color: "var(--off-white)" }}>
            {Messages.logIn()}
          </Button>
        )}
        <BugReportButton />
        <Tooltip title={Messages.discord()}>
          <IconButton
            component="a"
            href={DISCORD_INVITE_URL}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={Messages.discord()}
            css={css`
              opacity: 0.5;
            `}
            size="large"
          >
            <DiscordIcon />
          </IconButton>
        </Tooltip>
        <SoundsToggle />
        <Tooltip title={Messages.settings()}>
          <IconButton
            onClick={() => open()}
            css={css`
              opacity: 0.5;
              margin-right: 10px;
            `}
            size="large"
          >
            <SettingsOutlinedIcon />
          </IconButton>
        </Tooltip>
      </Box>
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
    </OuterBox>
  );
};

const trophyGlow = keyframes`
  0%, 100% { filter: drop-shadow(0 0 3px rgba(255, 196, 40, 0.55)); }
  50% { filter: drop-shadow(0 0 8px rgba(255, 196, 40, 0.95)); }
`;

/** A glowing golden trophy after the tab icons, sized like them: the community's tournaments, in
 * the browser. */
const TournamentsButton = () => (
  <Button
    component="a"
    href={TOURNAMENTS_URL}
    target="_blank"
    rel="noopener noreferrer"
    aria-label={Messages.tournaments()}
    css={css`
      color: #ffc83d;
      animation: ${trophyGlow} 2.4s ease-in-out infinite;
      &:hover {
        color: #ffd966;
        background-color: rgba(255, 196, 40, 0.12);
      }
      @media (prefers-reduced-motion: reduce) {
        animation: none;
        filter: drop-shadow(0 0 5px rgba(255, 196, 40, 0.8));
      }
    `}
  >
    <EmojiEventsRoundedIcon />
  </Button>
);

/** The bell: chat message and invite sounds on or off. */
const SoundsToggle = () => {
  const [on, setOn] = useNotificationSounds();
  return (
    <Tooltip title={Messages.toggleNotificationSounds()}>
      <IconButton
        onClick={() => void setOn(!on)}
        aria-pressed={on}
        aria-label={Messages.toggleNotificationSounds()}
        css={css`
          opacity: 0.5;
        `}
        size="large"
      >
        {on ? <NotificationsOutlinedIcon /> : <NotificationsOffOutlinedIcon />}
      </IconButton>
    </Tooltip>
  );
};

const PlayButton = ({ onClick }: { onClick: () => void }) => {
  const installStatus = useDolphinStore((store) => store.netplayStatus);
  const installProgress = useDolphinStore((store) => store.netplayDownloadProgress);
  const fillPercent = useMemo(() => {
    if (installStatus === DolphinStatus.READY) {
      return 1;
    }
    if (installProgress && installProgress.total > 0) {
      return installProgress.current / installProgress.total;
    }
    return 0;
  }, [installProgress, installStatus]);
  const launcherUpdateVersion = useLauncherUpdateVersion();
  const settingUp = installStatus === DolphinStatus.DOWNLOADING && installProgress?.phase !== undefined;

  // A newer launcher is out: no Dolphin starts until it is installed, so the button offers it
  // (after a running set-up step such as Project+'s first download, whose progress it shows first).
  if (launcherUpdateVersion && !settingUp) {
    return <LauncherUpdate />;
  }

  if (installStatus === DolphinStatus.READY) {
    return <PlayButtonImpl onClick={onClick} />;
  }

  return <UpdatingButton onClick={onClick} fillPercent={fillPercent} phase={installProgress?.phase} />;
};

const CheckForUpdatesButton = () => {
  const { dolphinService } = useServices();
  const { updateDolphin } = useDolphinActions(dolphinService);
  const { checkForAppUpdates } = useAppUpdate();
  const { showInfo, showError } = useToasts();
  const [checkingForUpdates, setCheckingForUpdates] = React.useState(false);

  const checkForUpdatesHandler = useMemo(() => {
    const checkForUpdates = async () => {
      setCheckingForUpdates(true);
      try {
        const updateResult = await checkForAppUpdates();
        if (updateResult && !updateResult.updateAvailable) {
          showInfo(Messages.noUpdateAvailable());
        }
        await updateDolphin();
      } catch (err) {
        log.error(err);
        showError(Messages.failedToGetUpdates());
      } finally {
        setCheckingForUpdates(false);
      }
    };
    return debounce(checkForUpdates, 500);
  }, [checkForAppUpdates, updateDolphin, showInfo, showError]);

  return (
    <Tooltip title={checkingForUpdates ? Messages.checkingForUpdates() : Messages.checkForUpdates()}>
      <Button
        style={isMac ? { marginTop: 10 } : undefined}
        onClick={checkForUpdatesHandler}
        disabled={checkingForUpdates}
      >
        {/* Slippi shows its logo here. We have no logo (no invented art), so the product name is shown in
            the game's title font once the assets are extracted. */}
        <span
          css={css`
            font-family: ${titleFont};
            font-size: 18px;
            color: var(--off-white);
            text-transform: none;
            ${NARROWEST_HEADER} {
              font-size: 14px;
            }
          `}
        >
          {PRODUCT_NAME}
        </span>
      </Button>
    </Tooltip>
  );
};
