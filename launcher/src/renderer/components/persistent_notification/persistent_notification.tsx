import { defaultServiceUrls, MAC_DOWNLOAD_URL, PRODUCT_NAME } from "@common/product";
import { css } from "@emotion/react";
import styled from "@emotion/styled";
import ButtonBase from "@mui/material/ButtonBase";
import log from "electron-log";
import React, { useCallback, useState } from "react";

import { useAppStore } from "@/lib/hooks/use_app_store";
import { useAppUpdate } from "@/lib/hooks/use_app_update";

import { PersistentNotificationMessages as Messages } from "./persistent_notification.messages";

export const PersistentNotification = React.memo(() => {
  const updateVersion = useAppStore((store) => store.updateVersion);
  const updateReady = useAppStore((store) => store.updateReady);
  const updateDownloadFailed = useAppStore((store) => store.updateDownloadFailed);

  const { installAppUpdate } = useAppUpdate();
  const [isInstalling, setIsInstalling] = useState(false);
  const [installError, setInstallError] = useState<string | null>(null);

  const handleInstall = useCallback(async () => {
    setIsInstalling(true);
    setInstallError(null);

    const result = await installAppUpdate();

    if (!result.success) {
      setIsInstalling(false);
      setInstallError(result.error || "Unknown error");
    }
  }, [installAppUpdate]);

  const handleManualDownload = useCallback(() => {
    window.electron.shell.openExternal(defaultServiceUrls.launcherUpdates).catch(log.error);
  }, []);

  const handleDownload = useCallback(() => {
    window.electron.shell.openExternal(MAC_DOWNLOAD_URL).catch(log.error);
  }, []);

  // macOS while the build cannot update itself (MAC_SELF_UPDATE): the update is found but not
  // downloaded, and the same bar offers the website's download instead of a restart.
  if (window.electron.bootstrap.launcherUpdateMode === "download") {
    if (!updateVersion) {
      return null;
    }
    return (
      <Outer>
        <div
          css={css`
            display: flex;
            justify-content: center;
          `}
        >
          <span
            css={css`
              margin-right: 10px;
            `}
          >
            {Messages.versionIsNowAvailable(updateVersion)}
          </span>
          <RestartButton onClick={handleDownload}>{Messages.downloadProduct(PRODUCT_NAME)}</RestartButton>
        </div>
      </Outer>
    );
  }

  // The handleInstall callback should provide immediate feedback i.e. it should immediately restart the
  // launcher so I don't think it's worth showing an 'Installing...' message that would need to be localised.
  // Instead we'll just show nothing as the feedback.
  if (isInstalling) {
    return null;
  }

  if (installError) {
    return (
      <Outer>
        <span>{Messages.installFailed()}</span>
        <RestartButton onClick={handleManualDownload}>{Messages.downloadManually()}</RestartButton>
      </Outer>
    );
  }

  if (!updateVersion) {
    return null;
  }

  // Shown as soon as the update is found, since no Dolphin starts until it is installed.
  if (!updateReady) {
    if (updateDownloadFailed) {
      return (
        <Outer>
          <span>{Messages.versionIsNowAvailable(updateVersion)}</span>
          <RestartButton onClick={handleManualDownload}>{Messages.downloadManually()}</RestartButton>
        </Outer>
      );
    }
    return (
      <Outer>
        <div
          css={css`
            display: flex;
            justify-content: center;
          `}
        >
          {Messages.downloadingVersion(updateVersion)}
        </div>
      </Outer>
    );
  }

  return (
    <Outer>
      <div
        css={css`
          display: flex;
          justify-content: center;
        `}
      >
        <span
          css={css`
            margin-right: 10px;
          `}
        >
          {Messages.versionIsNowAvailable(updateVersion)}
        </span>
        <RestartButton disabled={isInstalling} onClick={handleInstall}>
          {Messages.installUpdate()}
        </RestartButton>
      </div>
    </Outer>
  );
});

const Outer = styled.div`
  display: flex;
  flex-direction: row;
  justify-content: center;
  align-items: center;
  gap: 10px;
  position: relative;
  height: 30px;
  background-color: var(--surface-3);
  text-align: center;
  font-size: 14px;
`;

const RestartButton = styled(ButtonBase)`
  font-weight: 500;
  padding: 0 5px;
  &:hover {
    opacity: 0.8;
  }
`;
