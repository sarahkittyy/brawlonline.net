import type { InputDisplayController } from "@common/input_display";
import { DEFAULT_INPUT_DISPLAY, INPUT_DISPLAY_CONTROLLERS, normalizeInputDisplay } from "@common/input_display";
import type { DolphinControllers } from "@dolphin/mods/types";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import FormControlLabel from "@mui/material/FormControlLabel";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import React from "react";

import { Toggle } from "@/components/form/toggle";
import { useInputDisplay } from "@/lib/hooks/use_settings";
import { useServices } from "@/services";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";
import { InputDisplayPreview } from "./input_display_preview";

const controllerName = (c: InputDisplayController) => {
  switch (c) {
    case "gc":
      return Messages.inputDisplayGameCube();
    case "wiimote":
      return Messages.inputDisplayWiiRemote();
    case "nunchuk":
      return Messages.inputDisplayNunchuk();
    case "classic":
      return Messages.inputDisplayClassic();
  }
};

/** The controller the player has set up in Dolphin (as Other Games picks: a Wii Remote only when
 * no GameCube port has one), with its extension. */
export function detectedController(controllers: DolphinControllers | null): InputDisplayController {
  if (!controllers || controllers.gc.length > 0 || controllers.wiimote.length === 0) {
    return "gc";
  }
  const extension = controllers.wiimote[0].extension;
  return extension === "nunchuk" ? "nunchuk" : extension === "classic" ? "classic" : "wiimote";
}

/**
 * Settings > Game > Input Display (card 775): on or off, and where the box sits and how big it is,
 * on a preview of the game's picture with the player's own controller (another can be looked at).
 * Changes reach a running game at once.
 */
export const InputDisplaySetting = () => {
  const [stored, setStored] = useInputDisplay();
  const value = normalizeInputDisplay(stored);
  const { modsService } = useServices();
  const [controller, setController] = React.useState<InputDisplayController | null>(null);

  React.useEffect(() => {
    let alive = true;
    void modsService
      .refreshControllers()
      .then(() => modsService.getState())
      .then((state) => alive && setController((c) => c ?? detectedController(state.controllers)))
      .catch(() => alive && setController((c) => c ?? "gc"));
    return () => {
      alive = false;
    };
  }, [modsService]);

  const shown = controller ?? "gc";
  const moved =
    value.x !== DEFAULT_INPUT_DISPLAY.x ||
    value.y !== DEFAULT_INPUT_DISPLAY.y ||
    value.width !== DEFAULT_INPUT_DISPLAY.width;

  return (
    <SettingItem>
      <Toggle
        value={value.enabled}
        onChange={(enabled) => void setStored({ ...value, enabled })}
        label={Messages.inputDisplay()}
      />
      <div
        css={css`
          display: flex;
          flex-direction: column;
          gap: 8px;
          margin-top: 8px;
        `}
      >
        <InputDisplayPreview
          value={value}
          controller={shown}
          disabled={!value.enabled}
          onChange={(next) => void setStored({ ...value, x: next.x, y: next.y, width: next.width })}
        />
        <div
          css={css`
            display: flex;
            align-items: center;
            flex-wrap: wrap;
            gap: 8px;
          `}
        >
          <ToggleButtonGroup
            size="small"
            exclusive={true}
            value={shown}
            onChange={(_e, c: InputDisplayController | null) => c && setController(c)}
          >
            {INPUT_DISPLAY_CONTROLLERS.map((c) => (
              <ToggleButton
                key={c}
                value={c}
                css={css`
                  text-transform: none;
                `}
              >
                {controllerName(c)}
              </ToggleButton>
            ))}
          </ToggleButtonGroup>
          <FormControlLabel
            disabled={!value.enabled}
            control={
              <Checkbox
                size="small"
                checked={value.everyone}
                onChange={(e) => void setStored({ ...value, everyone: e.target.checked })}
              />
            }
            label={Messages.inputDisplayEveryone()}
          />
          {moved && (
            <Button
              color="secondary"
              disabled={!value.enabled}
              onClick={() =>
                void setStored({
                  ...value,
                  x: DEFAULT_INPUT_DISPLAY.x,
                  y: DEFAULT_INPUT_DISPLAY.y,
                  width: DEFAULT_INPUT_DISPLAY.width,
                })
              }
            >
              {Messages.inputDisplayReset()}
            </Button>
          )}
        </div>
      </div>
    </SettingItem>
  );
};
