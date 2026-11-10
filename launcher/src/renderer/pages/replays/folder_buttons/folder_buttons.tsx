/**
 * The buttons at the foot of Replays' and Highlights' sidebars: open the Root Replay Folder and the
 * rendered videos' folder (Settings > Clips) in the file manager.
 */
import FolderIcon from "@mui/icons-material/Folder";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { useSetting } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { defaultClipsFolder } from "@/pages/settings/clips_settings/clips_settings";

import { FolderButtonsMessages as Messages } from "./folder_buttons.messages";
import styles from "./folder_buttons.module.css";

export const FolderButtons = React.memo(({ className }: { className?: string }) => {
  const [rootSlpPath] = useSetting("rootSlpPath");
  const [clipsFolder] = useSetting("clipsFolder");
  const { showInfo } = useToasts();

  const openFolder = async (folder: string) => {
    // (Electron's error text, or "" when it opened.) The videos' folder is made with the first video.
    if (await window.electron.shell.openPath(folder)) {
      showInfo(Messages.folderNotThereYet(folder));
    }
  };
  const folderButton = (label: string, folder: string) => (
    <Tooltip title={folder} placement="right" arrow={true}>
      <button type="button" className={styles.button} onClick={() => void openFolder(folder)}>
        <FolderIcon fontSize="small" />
        <span className={styles.label}>{label}</span>
      </button>
    </Tooltip>
  );

  return (
    <div className={`${styles.buttons} ${className ?? ""}`}>
      {folderButton(Messages.openReplayLocation(), rootSlpPath)}
      {folderButton(Messages.openVideoLocation(), clipsFolder ?? defaultClipsFolder(rootSlpPath))}
    </div>
  );
});
