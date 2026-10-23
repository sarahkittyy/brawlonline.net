import {
  INPUT_DELAY_AGGRESSIVE,
  INPUT_DELAY_CHOICES,
  INPUT_DELAY_NORMAL,
  INPUT_DELAY_PROFILES,
  INPUT_DELAY_SMOOTH,
  isInputDelayProfile,
  normalizeInputDelay,
} from "@common/input_delay";
import { css } from "@emotion/react";
import FormControlLabel from "@mui/material/FormControlLabel";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Switch from "@mui/material/Switch";
import Typography from "@mui/material/Typography";
import React from "react";

import { useInputDelay } from "@/lib/hooks/use_settings";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";

// The number of frames the switch turns to Fixed with, before the player picked one.
const DEFAULT_FIXED_FRAMES = 2;

const profileLabel = (profile: number) =>
  profile === INPUT_DELAY_AGGRESSIVE
    ? Messages.inputDelayAggressive()
    : profile === INPUT_DELAY_SMOOTH
    ? Messages.inputDelaySmooth()
    : Messages.inputDelayNormal();

const SideLabel = ({ active, children }: { active: boolean; children: React.ReactNode }) => (
  <Typography
    css={css`
      font-weight: ${active ? 600 : 400};
      opacity: ${active ? 1 : 0.5};
    `}
  >
    {children}
  </Typography>
);

/**
 * "Input Delay": an Automatic <-> Fixed switch, then Automatic's profiles (Aggressive, Normal,
 * Smooth; picked from the ping at each game's start) or Fixed's 1-4 frames. Both apply in every
 * mode and right away, mid-game included (input-delay.json).
 */
export const InputDelaySetting = () => {
  const [inputDelay, setInputDelay] = useInputDelay();
  const value = normalizeInputDelay(inputDelay);
  const automatic = isInputDelayProfile(value);

  // Each side's last pick, so switching back and forth comes back to it.
  const lastProfile = React.useRef(automatic ? value : INPUT_DELAY_NORMAL);
  const lastFrames = React.useRef(automatic ? DEFAULT_FIXED_FRAMES : value);
  React.useEffect(() => {
    if (isInputDelayProfile(value)) {
      lastProfile.current = value;
    } else {
      lastFrames.current = value;
    }
  }, [value]);

  return (
    <SettingItem name={Messages.inputDelay()} description={Messages.inputDelayDescription()}>
      <div
        css={css`
          display: flex;
          align-items: center;
          gap: 4px;
          margin-bottom: 4px;
        `}
      >
        <SideLabel active={automatic}>{Messages.inputDelayAutomatic()}</SideLabel>
        <Switch
          checked={!automatic}
          onChange={(e) => void setInputDelay(e.target.checked ? lastFrames.current : lastProfile.current)}
          color="primary"
        />
        <SideLabel active={!automatic}>{Messages.inputDelayFixed()}</SideLabel>
      </div>
      <RadioGroup row={true} value={value} onChange={(_event, v) => void setInputDelay(Number(v))}>
        {automatic
          ? INPUT_DELAY_PROFILES.map((profile) => (
              <FormControlLabel key={profile} value={profile} label={profileLabel(profile)} control={<Radio />} />
            ))
          : INPUT_DELAY_CHOICES.map((frames) => (
              <FormControlLabel
                key={frames}
                value={frames}
                label={Messages.inputDelayFrames(frames)}
                control={<Radio />}
              />
            ))}
      </RadioGroup>
    </SettingItem>
  );
};
