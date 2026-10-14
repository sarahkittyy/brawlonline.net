import { INTERFACE_ZOOM_LEVELS as LEVELS } from "@common/interface_zoom";
import TextDecreaseRoundedIcon from "@mui/icons-material/TextDecreaseRounded";
import TextIncreaseRoundedIcon from "@mui/icons-material/TextIncreaseRounded";
import Slider from "@mui/material/Slider";
import Stack from "@mui/material/Stack";
import React, { useEffect, useState } from "react";

import { useInterfaceZoom } from "@/lib/hooks/use_settings";

import { SettingItem } from "../../setting_item_section";
import { InterfaceZoomMessages as Messages } from "./interface_zoom.messages";

// The slider stops only on the levels.
const MARKS = LEVELS.map((value) => ({ value }));

/** The launcher's size, 80-200 %; it changes when the slider is let go (it moves the slider too). */
export const InterfaceZoom = React.memo(() => {
  const [zoom, setZoom] = useInterfaceZoom();
  const [dragging, setDragging] = useState(zoom);
  useEffect(() => setDragging(zoom), [zoom]);

  return (
    <SettingItem name={Messages.interfaceZoom()} description={Messages.interfaceZoomDescription()}>
      <Stack direction="row" spacing={2} alignItems="center" sx={{ maxWidth: 360 }}>
        <TextDecreaseRoundedIcon fontSize="small" />
        <Slider
          aria-label={Messages.interfaceZoom()}
          value={dragging}
          min={LEVELS[0]}
          max={LEVELS[LEVELS.length - 1]}
          step={null}
          marks={MARKS}
          valueLabelDisplay="auto"
          valueLabelFormat={(v) => `${v}%`}
          onChange={(_, v) => setDragging(v as number)}
          onChangeCommitted={(_, v) => void setZoom(v as number)}
        />
        <TextIncreaseRoundedIcon fontSize="small" />
      </Stack>
    </SettingItem>
  );
});
