import { DolphinLaunchType } from "@dolphin/types";
import { css } from "@emotion/react";
import LinearProgress from "@mui/material/LinearProgress";
import Typography from "@mui/material/Typography";
import log from "electron-log";
import React from "react";

import { PathInput } from "@/components/path_input/path_input";
import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useGameFilesFolder, useSetting } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { humanReadableBytes } from "@/lib/utils";
import { useServices } from "@/services";

import { defaultClipsFolder } from "../clips_settings/clips_settings";
import { SettingItem } from "../setting_item_section";
import { DolphinSettingsMessages as Messages } from "./dolphin_settings.messages";

const isInside = (folder: string, root: string) => {
  const norm = (p: string) => p.replace(/[\\/]+$/, "").toLowerCase();
  const f = norm(folder);
  const r = norm(root);
  return f === r || f.startsWith(`${r}\\`) || f.startsWith(`${r}/`);
};

/**
 * The Brawl Online folder (dolphin/install/main_folder.ts): one folder for the game files (the
 * netplay folder, about 4 GB) and the replays, and a Move button that moves them all there
 * (DolphinManager.moveMainFolder). Below it, where each of them is now. The Play button shows the
 * move's progress too.
 */
export const MainFolder = () => {
  const [mainFolder] = useSetting("mainFolder");
  const gameFilesFolder = useGameFilesFolder();
  const [rootSlpPath] = useSetting("rootSlpPath");
  const [clipsFolder] = useSetting("clipsFolder");
  const [gameFiles, setGameFiles] = React.useState("");
  const [moving, setMoving] = React.useState(false);
  const netplayStatus = useDolphinStore((store) => store.netplayStatus);
  const dolphinOpen = useDolphinStore((store) => store.netplayOpened || store.playbackOpened);
  const progress = useDolphinStore((store) => store.netplayDownloadProgress);
  const { dolphinService } = useServices();
  const { showError, showSuccess, showWarning, showInfo } = useToasts();

  React.useEffect(() => {
    void dolphinService.getDolphinPaths(DolphinLaunchType.NETPLAY).then((paths) => setGameFiles(paths.gameFilesFolder));
  }, [dolphinService, gameFilesFolder]);

  const move = async (target: string) => {
    setMoving(true);
    try {
      const outcome = await dolphinService.moveMainFolder(target);
      switch (outcome.action) {
        case "error":
          showError(outcome.message);
          break;
        case "moved":
          showSuccess(Messages.mainFolderMoved(outcome.folder));
          if (outcome.replays === "failed") {
            showWarning(Messages.replaysNotMoved(outcome.replaysError ?? ""));
          } else if (outcome.replays === "kept" && !isInside(rootSlpPath, outcome.folder)) {
            showInfo(Messages.replaysKept(rootSlpPath));
          }
          if (outcome.leftBehind.length > 0) {
            showWarning(Messages.filesLeftBehind(outcome.leftBehind.join(", ")));
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
    <SettingItem name={Messages.mainFolder()} description={Messages.mainFolderDescription()}>
      <PathInput
        value={mainFolder ?? ""}
        placeholder={Messages.mainFolderNotChosen()}
        onSelect={(p) => void move(p)}
        disabled={disabled}
        tooltipText={disabledReason}
        buttonText={Messages.moveToMainFolder()}
        options={{ properties: ["openDirectory", "createDirectory", "promptToCreate"] }}
      />
      <Typography
        variant="caption"
        component="div"
        css={css`
          margin-top: 6px;
          opacity: 0.7;
          word-break: break-all;
        `}
      >
        <div>{Messages.gameFilesAt(gameFiles)}</div>
        <div>{Messages.replaysAt(rootSlpPath)}</div>
        <div>{Messages.clipsAt(clipsFolder ?? defaultClipsFolder(rootSlpPath))}</div>
      </Typography>
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
