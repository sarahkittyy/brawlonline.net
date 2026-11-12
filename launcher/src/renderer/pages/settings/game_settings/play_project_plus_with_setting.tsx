// Settings > Game > "Play Project+ with": Dolphin or EmissaryRecomp (docs/design/brawl-recompiled.md
// §4.2). It replaced "Play Button Action" (Launch Project+ / Launch Dolphin): Configure Dolphin in
// Settings > Netplay Dolphin opens Dolphin without a game. EmissaryRecomp's line says where it
// stands; picking it before it is set up opens Settings > Add-ons, and Play uses Dolphin until then.
import type { EmissaryState } from "@dolphin/emissary/types";
import FormControlLabel from "@mui/material/FormControlLabel";
import Link from "@mui/material/Link";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Tooltip from "@mui/material/Tooltip";
import React, { useEffect } from "react";
import { useNavigate } from "react-router-dom";

import { refreshEmissary, useEmissary, watchEmissary } from "@/lib/emissary/use_emissary";
import { usePlayProjectPlusWith } from "@/lib/hooks/use_settings";

import { AddonsSettingsMessages } from "../addons_settings/addons_settings.messages";
import { ADDONS_SETTINGS_PATH } from "../addons_settings/paths";
import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";

export type PlayProjectPlusWithViewProps = {
  value: "dolphin" | "emissary";
  emissary: EmissaryState | null;
  disabled?: boolean;
  disabledReason?: string;
  onChange: (value: "dolphin" | "emissary") => void;
  onSetUp: () => void;
};

/** EmissaryRecomp can play (or is playing): picking it needs nothing more. */
export const emissaryUsable = (s: EmissaryState | null) =>
  s != null && (s.status === "ready" || s.status === "updateAvailable" || s.status === "running");

export const PlayProjectPlusWithView = ({
  value,
  emissary,
  disabled,
  disabledReason,
  onChange,
  onSetUp,
}: PlayProjectPlusWithViewProps) => {
  const usable = emissaryUsable(emissary);
  const unavailable = emissary?.status === "unavailable";
  return (
    <SettingItem name={Messages.playProjectPlusWith()} description={Messages.playProjectPlusWithDescription()}>
      <Tooltip title={disabled ? disabledReason ?? "" : ""} placement="bottom-start">
        <RadioGroup
          value={value}
          onChange={(_event, v) => {
            const next = v === "emissary" ? "emissary" : "dolphin";
            onChange(next);
            if (next === "emissary" && !usable) {
              onSetUp();
            }
          }}
        >
          <FormControlLabel
            value="dolphin"
            label={Messages.playWithDolphin()}
            control={<Radio />}
            disabled={disabled}
          />
          <FormControlLabel
            value="emissary"
            disabled={disabled || unavailable}
            control={<Radio />}
            label={
              <span style={{ display: "inline-flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}>
                {Messages.playWithEmissary()}
                <span
                  style={{
                    fontSize: 11,
                    fontWeight: 700,
                    padding: "1px 6px",
                    borderRadius: 4,
                    border: "1px solid currentColor",
                  }}
                >
                  {Messages.beta()}
                </span>
                {emissary && !usable && (
                  <span style={{ fontSize: 13, opacity: 0.7 }} data-testid="emissary-choice-status">
                    {AddonsSettingsMessages.statusText(emissary.status)}{" "}
                    {!unavailable && (
                      <Link
                        component="button"
                        underline="hover"
                        onClick={(e: React.MouseEvent) => {
                          e.preventDefault();
                          onSetUp();
                        }}
                      >
                        {Messages.emissarySetUp()}
                      </Link>
                    )}
                  </span>
                )}
              </span>
            }
          />
        </RadioGroup>
      </Tooltip>
    </SettingItem>
  );
};

export const PlayProjectPlusWithSetting = ({ disabled }: { disabled?: boolean }) => {
  const [value, setValue] = usePlayProjectPlusWith();
  const emissary = useEmissary((s) => s.state);
  const navigate = useNavigate();
  useEffect(() => {
    watchEmissary();
    void refreshEmissary(false);
  }, []);
  return (
    <PlayProjectPlusWithView
      value={value}
      emissary={emissary}
      disabled={disabled}
      disabledReason={Messages.closeDolphinToChange()}
      onChange={(v) => void setValue(v)}
      onSetUp={() => navigate(ADDONS_SETTINGS_PATH)}
    />
  );
};
