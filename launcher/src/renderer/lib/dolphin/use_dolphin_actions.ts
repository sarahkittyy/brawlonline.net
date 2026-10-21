import type { DolphinService, RenderJobInfo, RenderRequest, ReplayQueueItem } from "@dolphin/types";
import { DolphinLaunchType } from "@dolphin/types";
import type { HighlightPlayItem } from "@replays/highlights/types";
import log from "electron-log";
import { useCallback } from "react";

import { useAppStore } from "@/lib/hooks/use_app_store";
import { useToasts } from "@/lib/hooks/use_toasts";
import { showWrongPPlusBuild } from "@/lib/pplus/pplus_build_dialog";
import { withRenderVersion, withReplayVersion } from "@/lib/pplus/with_replay_version";

import { DolphinMessages as Messages } from "./dolphin.messages";
import { DolphinStatus, setDolphinOpened, useDolphinStore } from "./use_dolphin_store";

export const useDolphinActions = (dolphinService: DolphinService) => {
  const { showError, showInfo } = useToasts();
  const netplayStatus = useDolphinStore((store) => store.netplayStatus);
  const playbackStatus = useDolphinStore((store) => store.playbackStatus);

  const getInstallStatus = useCallback(
    (dolphinType: DolphinLaunchType): DolphinStatus => {
      switch (dolphinType) {
        case DolphinLaunchType.NETPLAY:
          return netplayStatus;
        case DolphinLaunchType.PLAYBACK:
          return playbackStatus;
      }
    },
    [netplayStatus, playbackStatus],
  );

  /**
   * False (with the error shown) while a newer launcher is out: no new Dolphin starts until the
   * launcher is updated (main checks the same in DolphinManager's launch gate). A failed check
   * does not block.
   */
  const launcherIsUpToDate = useCallback(async (): Promise<boolean> => {
    let version: string | null = null;
    try {
      version = await window.electron.common.requiredAppUpdate();
    } catch (err) {
      log.warn(err);
    }
    if (version) {
      useAppStore.getState().setUpdateVersion(version);
      showError(Messages.updateLauncherFirst(version));
      return false;
    }
    return true;
  }, [showError]);

  const updateDolphin = useCallback(async () => {
    return Promise.all(
      [DolphinLaunchType.NETPLAY, DolphinLaunchType.PLAYBACK].map(async (dolphinType) => {
        if (getInstallStatus(dolphinType) !== DolphinStatus.READY) {
          return;
        }
        return dolphinService.downloadDolphin(dolphinType).catch((err) => {
          log.error(err);
          const dolphinTypeName =
            dolphinType === DolphinLaunchType.NETPLAY ? Messages.netplayDolphin() : Messages.playbackDolphin();
          showError(Messages.failedToInstallDolphin(dolphinTypeName));
        });
      }),
    );
  }, [getInstallStatus, dolphinService, showError]);

  const openConfigureDolphin = useCallback(
    async (dolphinType: DolphinLaunchType) => {
      if (getInstallStatus(dolphinType) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return;
      }
      if (!(await launcherIsUpToDate())) {
        return;
      }

      dolphinService
        .configureDolphin(dolphinType)
        .then(() => {
          setDolphinOpened(dolphinType);
        })
        .catch(showError);
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError],
  );

  const softResetDolphin = useCallback(
    async (dolphinType: DolphinLaunchType) => {
      try {
        await dolphinService.softResetDolphin(dolphinType);
      } catch (err) {
        showError(err);
      }
    },
    [dolphinService, showError],
  );

  const hardResetDolphin = useCallback(
    async (dolphinType: DolphinLaunchType) => {
      try {
        await dolphinService.hardResetDolphin(dolphinType);
      } catch (err) {
        showError(err);
      }
    },
    [dolphinService, showError],
  );

  /**
   * Starts the netplay Dolphin; resolves to whether it started (errors are shown). `bootGame` boots
   * P+ even when the Play button is set to open Dolphin only.
   */
  const launchNetplay = useCallback(
    async (bootGame?: boolean): Promise<boolean> => {
      if (getInstallStatus(DolphinLaunchType.NETPLAY) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return false;
      }
      if (!(await launcherIsUpToDate())) {
        return false;
      }

      try {
        const result = await dolphinService.launchNetplayDolphin({ bootGame });
        if (result.status === "wrongPPlusBuild") {
          // The SD card is on another Project+ release: the popup offers the right one.
          showWrongPPlusBuild({ cardRelease: result.cardRelease, release: result.release, bootGame });
          return false;
        }
        setDolphinOpened(DolphinLaunchType.NETPLAY);
        return true;
      } catch (err) {
        showError(err);
        return false;
      }
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError],
  );

  const viewReplays = useCallback(
    async (...files: ReplayQueueItem[]) => {
      if (getInstallStatus(DolphinLaunchType.PLAYBACK) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return;
      }
      if (!(await launcherIsUpToDate())) {
        return;
      }

      // On the Project+ release they were recorded on (with_replay_version.ts asks for its files).
      withReplayVersion((choice) => dolphinService.viewSlpReplay(files, choice), showInfo)
        .then((result) => {
          if (result?.status === "playing") {
            setDolphinOpened(DolphinLaunchType.PLAYBACK);
          }
        })
        .catch(showError);
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError, showInfo],
  );

  /**
   * Plays highlights in the playback Dolphin (the next one at once: an open Dolphin is not started
   * again, so the launcher's update check is only made when it is not open). Resolves to whether the
   * request was made.
   */
  const playHighlights = useCallback(
    async (items: HighlightPlayItem[], commandId: string): Promise<boolean> => {
      if (getInstallStatus(DolphinLaunchType.PLAYBACK) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return false;
      }
      if (!useDolphinStore.getState().playbackOpened && !(await launcherIsUpToDate())) {
        return false;
      }
      try {
        const result = await withReplayVersion((choice) => dolphinService.playHighlights(items, commandId, choice));
        if (result?.status !== "playing") {
          return false;
        }
        setDolphinOpened(DolphinLaunchType.PLAYBACK);
        return true;
      } catch (err) {
        showError(err);
        return false;
      }
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError],
  );

  /**
   * Renders highlights to video (no Dolphin window; progress as RENDER_PROGRESS events). Resolves to
   * the jobs queued, or null when nothing was (an error is shown, or the player cancelled).
   */
  const renderHighlights = useCallback(
    async (request: RenderRequest): Promise<RenderJobInfo[] | null> => {
      if (getInstallStatus(DolphinLaunchType.PLAYBACK) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return null;
      }
      if (!(await launcherIsUpToDate())) {
        return null;
      }
      try {
        return await withRenderVersion((choice) => dolphinService.renderHighlights(request, choice));
      } catch (err) {
        showError(err);
        return null;
      }
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError],
  );

  return {
    openConfigureDolphin,
    softResetDolphin,
    hardResetDolphin,
    launchNetplay,
    viewReplays,
    playHighlights,
    renderHighlights,
    updateDolphin,
  };
};
