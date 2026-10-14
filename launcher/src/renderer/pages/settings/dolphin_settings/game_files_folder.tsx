import { DolphinLaunchType } from "@dolphin/types";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import LinearProgress from "@mui/material/LinearProgress";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import log from "electron-log";
import React from "react";

import { PathInput } from "@/components/path_input/path_input";
import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useGameFilesFolder } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { humanReadableBytes } from "@/lib/utils";
import { useServices } from "@/services";

import { SettingItem } from "../setting_item_section";
import { DolphinSettingsMessages as Messages } from "./dolphin_settings.messages";

/**
 * Where the game files (the netplay folder, about 4 GB) are, and a Move button that moves them to
 * another folder (DolphinManager.moveGameFiles). The Play button shows the move's progress too.
 */
export const GameFilesFolder = () => {
  const gameFilesFolder = useGameFilesFolder();
  const [folder, setFolder] = React.useState("");
  const [moving, setMoving] = React.useState(false);
  const netplayStatus = useDolphinStore((store) => store.netplayStatus);
  const dolphinOpen = useDolphinStore((store) => store.netplayOpened || store.playbackOpened);
  const progress = useDolphinStore((store) => store.netplayDownloadProgress);
  const { dolphinService } = useServices();
  const { showError, showSuccess, showWarning } = useToasts();

  React.useEffect(() => {
    void dolphinService.getDolphinPaths(DolphinLaunchType.NETPLAY).then((paths) => setFolder(paths.gameFilesFolder));
  }, [dolphinService, gameFilesFolder]);

  const move = async (target: string | null) => {
    setMoving(true);
    try {
      const outcome = await dolphinService.moveGameFiles(target);
      switch (outcome.action) {
        case "error":
          showError(outcome.message);
          break;
        case "moved":
        case "adopted":
          setFolder(outcome.folder);
          showSuccess(Messages.gameFilesMoved(outcome.folder));
          if (outcome.leftBehind) {
            showWarning(Messages.gameFilesLeftBehind(outcome.leftBehind));
          }
          break;
      }
    } catch (err) {
      log.error(err);
      showError(err);
    } finally {
      setMoving(false);
    }
  };

  const disabledReason = dolphinOpen
    ? Messages.closeDolphinToChange()
    : netplayStatus !== DolphinStatus.READY && !moving
    ? Messages.waitForSetup()
    : "";
  const disabled = moving || disabledReason !== "";
  // A move shows its progress here whichever page started it.
  const moveProgress = progress?.phase === "moveGameFiles" ? progress : undefined;

  return (
    <SettingItem name={Messages.gameFilesFolder()} description={Messages.gameFilesFolderDescription()}>
      <PathInput
        value={folder}
        onSelect={(p) => void move(p)}
        disabled={disabled}
        tooltipText={disabledReason}
        buttonText={Messages.moveGameFiles()}
        options={{ properties: ["openDirectory", "createDirectory", "promptToCreate"] }}
        endAdornment={
          gameFilesFolder ? (
            <Tooltip title={disabledReason}>
              <span>
                <Button size="small" onClick={() => void move(null)} disabled={disabled}>
                  {Messages.defaultFolderButton()}
                </Button>
              </span>
            </Tooltip>
          ) : undefined
        }
      />
      {(moving || moveProgress) && (
        <div
          css={css`
            margin-top: 10px;
          `}
        >
          <LinearProgress
            variant={moveProgress && moveProgress.total > 0 ? "determinate" : "indeterminate"}
            value={moveProgress && moveProgress.total > 0 ? (moveProgress.current / moveProgress.total) * 100 : 0}
          />
          <Typography variant="caption">
            {moveProgress
              ? Messages.movingGameFiles(
                  humanReadableBytes(moveProgress.current),
                  humanReadableBytes(moveProgress.total),
                )
              : Messages.movingGameFilesNoBytes()}
          </Typography>
        </div>
      )}
    </SettingItem>
  );
};
