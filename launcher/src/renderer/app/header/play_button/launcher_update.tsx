import { launcherDownloadUrl, MAC_DOWNLOAD_URL } from "@common/product";
import log from "electron-log";
import React, { useCallback, useState } from "react";

import { useAppStore } from "@/lib/hooks/use_app_store";
import { useAppUpdate } from "@/lib/hooks/use_app_update";
import { useToasts } from "@/lib/hooks/use_toasts";

import { LauncherUpdateMessages as Messages } from "./launcher_update.messages";
import { LauncherUpdateButton } from "./play_button";

/** The newer launcher version that is out ("" while none is): the Play button offers it instead. */
export const useLauncherUpdateVersion = () => useAppStore((store) => store.updateVersion);

/**
 * The Play button while a newer launcher is out (it was a bar under the header, Slippi's). In
 * "install" mode it fills as the update downloads, then installs it and restarts the launcher; a
 * download or install that failed opens the website's installer instead. On macOS while the build
 * cannot update itself ("download" mode, MAC_SELF_UPDATE) it opens the website's download.
 */
export const LauncherUpdate = React.memo(() => {
  const updateVersion = useAppStore((store) => store.updateVersion);
  const updateReady = useAppStore((store) => store.updateReady);
  const updateDownloadProgress = useAppStore((store) => store.updateDownloadProgress);
  const updateDownloadFailed = useAppStore((store) => store.updateDownloadFailed);

  const { installAppUpdate } = useAppUpdate();
  const { showError } = useToasts();
  const [isInstalling, setIsInstalling] = useState(false);
  const [installFailed, setInstallFailed] = useState(false);

  // The website's installer for this platform (the update feed's folder is not browsable).
  const openManualDownload = useCallback(() => {
    const { isMac, isLinux } = window.electron.bootstrap;
    const url = launcherDownloadUrl(isMac ? "darwin" : isLinux ? "linux" : "win32");
    window.electron.shell.openExternal(url).catch(log.error);
  }, []);

  const install = useCallback(async () => {
    setIsInstalling(true);
    const result = await installAppUpdate();
    // On success the launcher restarts, so the button stays disabled until then.
    if (!result.success) {
      setIsInstalling(false);
      setInstallFailed(true);
      showError(Messages.installFailed());
    }
  }, [installAppUpdate, showError]);

  const downloadMode = window.electron.bootstrap.launcherUpdateMode === "download";
  const manual = !downloadMode && (installFailed || (!updateReady && updateDownloadFailed));
  const onClick = useCallback(() => {
    if (downloadMode) {
      window.electron.shell.openExternal(MAC_DOWNLOAD_URL).catch(log.error);
    } else if (manual) {
      openManualDownload();
    } else {
      void install();
    }
  }, [downloadMode, manual, openManualDownload, install]);

  if (!updateVersion) {
    return null;
  }
  // Found and downloading (shown at once: no Dolphin starts until it is installed); the
  // percentage shows a ~100 MB installer on a slow line is moving.
  const downloading = !downloadMode && !manual && !updateReady;
  return (
    <LauncherUpdateButton
      currentVersion={__VERSION__}
      newVersion={updateVersion}
      downloadProgress={downloading ? updateDownloadProgress / 100 : undefined}
      disabled={isInstalling}
      onClick={onClick}
      title={manual ? Messages.downloadManually() : undefined}
    />
  );
});
