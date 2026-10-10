import type { GeckoCode } from "@dolphin/config/gecko_code";
import type { DolphinService, ReplayQueueItem } from "@dolphin/types";
import { DolphinLaunchType } from "@dolphin/types";
import log from "electron-log";
import { useCallback } from "react";

import { useAppStore } from "@/lib/hooks/use_app_store";
import { useToasts } from "@/lib/hooks/use_toasts";

import { DolphinMessages as Messages } from "./dolphin.messages";
import { DolphinStatus, setDolphinOpened, useDolphinStore } from "./use_dolphin_store";

export const useDolphinActions = (dolphinService: DolphinService) => {
  const { showError } = useToasts();
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

  /** Starts the netplay Dolphin; resolves to whether it started (errors are shown). */
  const launchNetplay = useCallback(async (): Promise<boolean> => {
    if (getInstallStatus(DolphinLaunchType.NETPLAY) !== DolphinStatus.READY) {
      showError(Messages.dolphinIsUpdating());
      return false;
    }
    if (!(await launcherIsUpToDate())) {
      return false;
    }

    try {
      await dolphinService.launchNetplayDolphin();
      setDolphinOpened(DolphinLaunchType.NETPLAY);
      return true;
    } catch (err) {
      showError(err);
      return false;
    }
  }, [getInstallStatus, launcherIsUpToDate, dolphinService, showError]);

  const viewReplays = useCallback(
    async (...files: ReplayQueueItem[]) => {
      if (getInstallStatus(DolphinLaunchType.PLAYBACK) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return;
      }
      if (!(await launcherIsUpToDate())) {
        return;
      }

      dolphinService
        .viewSlpReplay(files)
        .then(() => {
          setDolphinOpened(DolphinLaunchType.PLAYBACK);
        })
        .catch(showError);
    },
    [getInstallStatus, launcherIsUpToDate, dolphinService, showError],
  );

  const readGeckoCodes = useCallback(
    async (dolphinType: DolphinLaunchType) => {
      if (getInstallStatus(dolphinType) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return;
      }
      return await dolphinService.fetchGeckoCodes(dolphinType);
    },
    [dolphinService, getInstallStatus, showError],
  );

  const saveGeckoCodes = useCallback(
    async (dolphinType: DolphinLaunchType, geckoCodes: GeckoCode[]) => {
      if (getInstallStatus(dolphinType) !== DolphinStatus.READY) {
        showError(Messages.dolphinIsUpdating());
        return;
      }

      return await dolphinService.saveGeckoCodes(dolphinType, geckoCodes);
    },
    [dolphinService, getInstallStatus, showError],
  );

  return {
    openConfigureDolphin,
    softResetDolphin,
    hardResetDolphin,
    launchNetplay,
    viewReplays,
    updateDolphin,
    readGeckoCodes,
    saveGeckoCodes,
  };
};
