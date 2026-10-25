import { DolphinLaunchType } from "@dolphin/types";
import { css } from "@emotion/react";
import FolderIcon from "@mui/icons-material/Folder";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Tooltip from "@mui/material/Tooltip";
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
 * (DolphinManager.moveMainFolder). Until all of them are in it, a folder button for each opens
 * where it is now (its path on hover). The Play button shows the move's progress too.
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

  const clips = clipsFolder ?? defaultClipsFolder(rootSlpPath);
  // (The game files' setting, not the folder asked for above: known at once, so nothing flashes.)
  const allInMainFolder =
    mainFolder != null &&
    gameFilesFolder != null &&
    [gameFilesFolder, rootSlpPath, clips].every((folder) => isInside(folder, mainFolder));
  const folders = [
    { name: Messages.gameFilesButton(), folder: gameFiles },
    { name: Messages.replaysButton(), folder: rootSlpPath },
    { name: Messages.clipsButton(), folder: clips },
  ];

  const open = async (folder: string) => {
    // (Electron's error text, or "" when it opened.) The clips folder is made with the first video.
    if (folder && (await window.electron.shell.openPath(folder))) {
      showInfo(Messages.folderNotThereYet(folder));
    }
  };

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
  // A move shows its progress here whichever page started it. (The last step's progress stays in
  // the store once it is done, as the Play button only reads it while setting up.)
  const moveProgress =
    netplayStatus === DolphinStatus.DOWNLOADING && progress?.phase === "moveGameFiles" ? progress : undefined;

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
        endAdornment={
          mainFolder ? (
            <Tooltip title={Messages.openMainFolder()}>
              <IconButton size="small" onClick={() => void open(mainFolder)}>
                <FolderIcon fontSize="small" />
              </IconButton>
            </Tooltip>
          ) : undefined
        }
      />
      {!allInMainFolder && (
        <div
          css={css`
            margin-top: 6px;
            display: flex;
            gap: 8px;
          `}
        >
          {folders.map(({ name, folder }) => (
            <Tooltip key={name} title={folder}>
              <Button
                size="small"
                color="inherit"
                startIcon={<FolderIcon />}
                onClick={() => void open(folder)}
                css={css`
                  opacity: 0.7;
                  text-transform: none;
                `}
              >
                {name}
              </Button>
            </Tooltip>
          ))}
        </div>
      )}
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
