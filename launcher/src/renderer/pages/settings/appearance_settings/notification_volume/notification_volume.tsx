import VolumeDownRoundedIcon from "@mui/icons-material/VolumeDownRounded";
import VolumeUpRoundedIcon from "@mui/icons-material/VolumeUpRounded";
import Slider from "@mui/material/Slider";
import Stack from "@mui/material/Stack";
import React, { useEffect, useState } from "react";

import { useNotificationVolume } from "@/lib/hooks/use_settings";
import { playSound } from "@/lib/social/sounds";

import { SettingItem } from "../../setting_item_section";
import { NotificationVolumeMessages as Messages } from "./notification_volume.messages";

/** The chat message and invite sounds' volume, 0-100 %; letting go plays the message sound at it. */
export const NotificationVolume = React.memo(() => {
  const [volume, setVolume] = useNotificationVolume();
  const [dragging, setDragging] = useState(volume);
  useEffect(() => setDragging(volume), [volume]);

  return (
    <SettingItem name={Messages.notificationVolume()} description={Messages.notificationVolumeDescription()}>
      <Stack direction="row" spacing={2} alignItems="center" sx={{ maxWidth: 360 }}>
        <VolumeDownRoundedIcon fontSize="small" />
        <Slider
          aria-label={Messages.notificationVolume()}
          value={dragging}
          min={0}
          max={100}
          step={5}
          valueLabelDisplay="auto"
          valueLabelFormat={(v) => `${v}%`}
          onChange={(_, v) => setDragging(v as number)}
          onChangeCommitted={(_, v) => {
            void setVolume(v as number);
            playSound("chat-message", { preview: true, volume: v as number });
          }}
        />
        <VolumeUpRoundedIcon fontSize="small" />
      </Stack>
    </SettingItem>
  );
});
