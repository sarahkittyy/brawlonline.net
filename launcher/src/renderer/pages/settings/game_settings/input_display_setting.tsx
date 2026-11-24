import type { InputDisplayController } from "@common/input_display";
import { normalizeInputDisplay } from "@common/input_display";
import type { DolphinControllers } from "@dolphin/mods/types";
import { css } from "@emotion/react";
import React from "react";

import { Toggle } from "@/components/form/toggle";
import { useInputDisplay } from "@/lib/hooks/use_settings";
import { useServices } from "@/services";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";
import { InputDisplayPreview } from "./input_display_preview";

/** The controller the player has set up in Dolphin (as Other Games picks: a Wii Remote only when
 * no GameCube port has one), with its extension: what the preview draws. The game draws the one
 * they actually play on. */
export function detectedController(controllers: DolphinControllers | null): InputDisplayController {
  if (!controllers || controllers.gc.length > 0 || controllers.wiimote.length === 0) {
    return "gc";
  }
  const extension = controllers.wiimote[0].extension;
  return extension === "nunchuk" ? "nunchuk" : extension === "classic" ? "classic" : "wiimote";
}

/**
 * Settings > Game > Input Display (card 775): on or off; while on, a preview of the game's picture
 * with the box, dragged to move it and resized by its corner. Changes reach a running game at once.
 */
export const InputDisplaySetting = () => {
  const [stored, setStored] = useInputDisplay();
  const value = normalizeInputDisplay(stored);
  const { modsService } = useServices();
  const [controller, setController] = React.useState<InputDisplayController>("gc");

  React.useEffect(() => {
    let alive = true;
    void modsService
      .refreshControllers()
      .then(() => modsService.getState())
      .then((state) => alive && setController(detectedController(state.controllers)))
      .catch(() => undefined);
    return () => {
      alive = false;
    };
  }, [modsService]);

  return (
    <SettingItem>
      <Toggle
        value={value.enabled}
        onChange={(enabled) => void setStored({ ...value, enabled })}
        label={Messages.inputDisplay()}
      />
      {value.enabled && (
        <div
          css={css`
            margin-top: 10px;
          `}
        >
          <InputDisplayPreview
            value={value}
            controller={controller}
            onChange={(next) => void setStored({ ...value, x: next.x, y: next.y, width: next.width })}
          />
        </div>
      )}
    </SettingItem>
  );
};
