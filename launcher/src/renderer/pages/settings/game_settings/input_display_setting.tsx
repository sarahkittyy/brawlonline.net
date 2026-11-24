import type { InputDisplayController, InputDisplaySettings } from "@common/input_display";
import { normalizeInputDisplay } from "@common/input_display";
import type { DolphinControllers } from "@dolphin/mods/types";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import Switch from "@mui/material/Switch";
import Typography from "@mui/material/Typography";
import React from "react";

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

/** The preview of the game's picture: the box is dragged and resized here, kept on Confirm. */
const CustomizeDialog = ({
  open,
  value,
  controller,
  onClose,
  onConfirm,
}: {
  open: boolean;
  value: InputDisplaySettings;
  controller: InputDisplayController;
  onClose: () => void;
  onConfirm: (value: InputDisplaySettings) => void;
}) => {
  const [draft, setDraft] = React.useState(value);
  React.useEffect(() => {
    if (open) {
      setDraft(value);
    }
    // Only as it opens: the setting changing meanwhile does not undo the player's drag.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);
  const changed = draft.x !== value.x || draft.y !== value.y || draft.width !== value.width;

  return (
    <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth={true}>
      <DialogContent
        css={css`
          padding-top: 24px;
        `}
        // MUI's own overflow wins over the css prop: the preview's corner handle would scroll it.
        style={{ overflowX: "hidden" }}
      >
        <InputDisplayPreview value={draft} controller={controller} onChange={setDraft} />
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} color="secondary">
          {Messages.inputDisplayCancel()}
        </Button>
        <Button
          onClick={() => {
            onConfirm(draft);
            onClose();
          }}
          color="primary"
          disabled={!changed}
        >
          {Messages.inputDisplayConfirm()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};

/**
 * Settings > Game > Input Display (card 775): its switch, and Customize, which opens the preview of
 * the game's picture with the box to drag and resize (Confirm keeps it). Changes reach a running
 * game at once.
 */
export const InputDisplaySetting = () => {
  const [stored, setStored] = useInputDisplay();
  const value = normalizeInputDisplay(stored);
  const { modsService } = useServices();
  const [controller, setController] = React.useState<InputDisplayController>("gc");
  const [customizing, setCustomizing] = React.useState(false);

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
      {/* Laid out like Chat Key: the name on the left, the controls on the right. */}
      <div
        css={css`
          display: flex;
          align-items: center;
          justify-content: space-between;
          gap: 10px;
        `}
      >
        <Typography>{Messages.inputDisplay()}</Typography>
        <div
          css={css`
            display: flex;
            align-items: center;
            gap: 10px;
            /* The switch's track sits inside its 12 px padding. */
            margin-right: 12px;
          `}
        >
          <Switch
            checked={value.enabled}
            onChange={(e) => void setStored({ ...value, enabled: e.target.checked })}
            color="primary"
            inputProps={{ "aria-label": Messages.inputDisplay() }}
          />
          <Button
            variant="outlined"
            color="secondary"
            onClick={() => setCustomizing(true)}
            css={css`
              min-width: 140px;
              text-transform: none;
            `}
          >
            {Messages.inputDisplayCustomize()}
          </Button>
        </div>
      </div>
      <CustomizeDialog
        open={customizing}
        value={value}
        controller={controller}
        onClose={() => setCustomizing(false)}
        onConfirm={(next) => void setStored({ ...value, x: next.x, y: next.y, width: next.width })}
      />
    </SettingItem>
  );
};
