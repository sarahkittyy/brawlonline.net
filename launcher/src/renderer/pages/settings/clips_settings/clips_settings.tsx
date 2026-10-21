import { CLIP_RESOLUTIONS, clipResolution } from "@common/clip_resolution";
import Button from "@mui/material/Button";
import React from "react";

import { Dropdown } from "@/components/form/dropdown";
import { Toggle } from "@/components/form/toggle";
import { PathInput } from "@/components/path_input/path_input";
import { useSetting } from "@/lib/hooks/use_settings";

import { SettingItem } from "../setting_item_section";
import { ClipsSettingsMessages as Messages } from "./clips_settings.messages";

/** The default video folder: `<Root Replay Folder>/Highlights` (SettingsManager.getClipsFolder). */
export const defaultClipsFolder = (rootSlpPath: string): string => {
  const sep = rootSlpPath.includes("\\") ? "\\" : "/";
  return `${rootSlpPath.replace(/[\\/]+$/, "")}${sep}Highlights`;
};

export const ClipsSettings = React.memo(() => {
  const [clipOnLrz, setClipOnLrz] = useSetting("clipOnLrz");
  const [disableDetection, setDisableDetection] = useSetting("disableHighlightDetection");
  const [clipsFolder, setClipsFolder] = useSetting("clipsFolder");
  const [rootSlpPath] = useSetting("rootSlpPath");
  const [resolution, setResolution] = useSetting("clipResolution");

  return (
    <div>
      <SettingItem>
        <Toggle
          value={clipOnLrz}
          onChange={(checked) => setClipOnLrz(checked)}
          label={Messages.clipOnLrz()}
          description={Messages.clipOnLrzDescription()}
        />
      </SettingItem>
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
