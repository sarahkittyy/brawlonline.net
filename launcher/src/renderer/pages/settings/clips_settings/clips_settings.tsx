import { currentClipButtons, withClipHotkey } from "@common/clip_hotkey";
import { CLIP_RESOLUTIONS, clipResolution } from "@common/clip_resolution";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import React from "react";

import { Checkbox } from "@/components/form/checkbox";
import { Dropdown } from "@/components/form/dropdown";
import { Toggle } from "@/components/form/toggle";
import { GcButtonRow } from "@/components/gc_button_icons/gc_button_icons";
import { PathInput } from "@/components/path_input/path_input";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useSetting } from "@/lib/hooks/use_settings";

import { SettingItem } from "../setting_item_section";
import { ClipHotkeyDialog } from "./clip_hotkey_dialog";
import { ClipsSettingsMessages as Messages } from "./clips_settings.messages";

/** The default video folder: `<Root Replay Folder>/Highlights` (SettingsManager.getClipsFolder). */
export const defaultClipsFolder = (rootSlpPath: string): string => {
  const sep = rootSlpPath.includes("\\") ? "\\" : "/";
  return `${rootSlpPath.replace(/[\\/]+$/, "")}${sep}Highlights`;
};

/**
 * Manual Clip Hotkey: Enable, Configure (ClipHotkeyDialog) and the hotkey now. A new hotkey is
 * added to the setting's history (@common/clip_hotkey), so earlier games keep the clips of theirs.
 */
const ClipHotkeySetting = () => {
  const [clipOnLrz, setClipOnLrz] = useSetting("clipOnLrz");
  const [clipHotkeys, setClipHotkeys] = useSetting("clipHotkeys");
  // The adapter is the netplay Dolphin's while it is open.
  const netplayDolphinOpen = useDolphinStore((store) => store.netplayOpened);
  const [configuring, setConfiguring] = React.useState(false);

  return (
    <SettingItem name={Messages.clipHotkey()} description={Messages.clipHotkeyDescription()}>
      <div
        css={css`
          display: flex;
          align-items: center;
          gap: 24px;
        `}
      >
        <Checkbox
          label={Messages.clipHotkeyEnable()}
          checked={clipOnLrz}
          onChange={(_, checked) => setClipOnLrz(checked)}
        />
        <Tooltip title={netplayDolphinOpen ? Messages.closeDolphinToConfigure() : ""}>
          <span>
            <Button
              variant="outlined"
              color="secondary"
              size="small"
              disabled={netplayDolphinOpen}
              onClick={() => setConfiguring(true)}
            >
              {Messages.clipHotkeyConfigure()}
            </Button>
          </span>
        </Tooltip>
      </div>
      <div
        css={css`
          display: flex;
          align-items: center;
          gap: 12px;
          margin-top: 12px;
          opacity: ${clipOnLrz ? 1 : 0.5};
        `}
      >
        <Typography variant="body2">{Messages.clipHotkeyCurrent()}</Typography>
        <GcButtonRow buttons={currentClipButtons(clipHotkeys)} />
      </div>
      <ClipHotkeyDialog
        open={configuring}
        onClose={() => setConfiguring(false)}
        onApply={(buttons) => setClipHotkeys(withClipHotkey(clipHotkeys, buttons, Date.now()))}
      />
    </SettingItem>
  );
};

export const ClipsSettings = React.memo(() => {
  const [disableDetection, setDisableDetection] = useSetting("disableHighlightDetection");
  const [clipsFolder, setClipsFolder] = useSetting("clipsFolder");
  const [rootSlpPath] = useSetting("rootSlpPath");
  const [resolution, setResolution] = useSetting("clipResolution");

  return (
    <div>
      <ClipHotkeySetting />
      <SettingItem>
        <Toggle
          value={disableDetection}
          onChange={(checked) => setDisableDetection(checked)}
          label={Messages.disableHighlightDetection()}
          description={Messages.disableHighlightDetectionDescription()}
        />
      </SettingItem>
      <SettingItem name={Messages.resolution()} description={Messages.resolutionDescription()}>
        <Dropdown
          value={clipResolution(resolution)}
          options={CLIP_RESOLUTIONS.map((r) => ({ value: r, label: Messages.resolutionOption(r) }))}
          onChange={(r) => void setResolution(r)}
        />
      </SettingItem>
      <SettingItem name={Messages.videoFolder()} description={Messages.videoFolderDescription()}>
        <PathInput
          value={clipsFolder ?? defaultClipsFolder(rootSlpPath)}
          onSelect={(folder) => setClipsFolder(folder)}
          options={{ properties: ["openDirectory", "createDirectory"] }}
          endAdornment={
            clipsFolder != null ? (
              <Button size="small" onClick={() => setClipsFolder(null)}>
                {Messages.resetToDefault()}
              </Button>
            ) : undefined
          }
        />
      </SettingItem>
    </div>
  );
});
