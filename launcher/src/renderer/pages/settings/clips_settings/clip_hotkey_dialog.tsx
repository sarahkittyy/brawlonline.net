import { DEFAULT_CLIP_BUTTONS } from "@common/clip_hotkey";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import Typography from "@mui/material/Typography";
import React from "react";

import { GcButtonRow } from "@/components/gc_button_icons/gc_button_icons";
import { GcControllerPicker } from "@/components/gc_button_icons/gc_controller_picker";

import { ClipsSettingsMessages as Messages } from "./clips_settings.messages";

/**
 * Configure for Manual Clip Hotkey: a GameCube controller whose buttons are clicked into and out
 * of the hotkey (Sarah's board, 2026-11-03). Nothing reads the player's controller: the hotkey is
 * GameCube buttons, whatever Dolphin maps to them (a keyboard, another pad).
 */
export const ClipHotkeyDialog = ({
  open,
  current,
  onClose,
  onApply,
}: {
  open: boolean;
  current: number;
  onClose: () => void;
  onApply: (buttons: number) => void;
}) => {
  const [buttons, setButtons] = React.useState(current);

  React.useEffect(() => {
    if (open) {
      setButtons(current);
    }
  }, [open, current]);

  return (
    <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth={true}>
      <DialogContent
        css={css`
          display: flex;
          flex-direction: column;
          align-items: center;
          gap: 12px;
          padding-top: 24px;
        `}
      >
        <Typography variant="subtitle1">{Messages.hotkeyClickButtons()}</Typography>
        <div
          css={css`
            width: 100%;
            max-width: 440px;
          `}
        >
          <GcControllerPicker buttons={buttons} onToggle={(bit) => setButtons((b) => b ^ bit)} />
        </div>
        <div
          css={css`
            min-height: 2.5rem;
            display: flex;
            align-items: center;
          `}
        >
          {buttons !== 0 ? (
            <GcButtonRow buttons={buttons} />
          ) : (
            <Typography
              variant="body2"
              css={css`
                opacity: 0.7;
              `}
            >
              {Messages.hotkeyNone()}
            </Typography>
          )}
        </div>
      </DialogContent>
      <DialogActions>
        <Button
          onClick={() => setButtons(DEFAULT_CLIP_BUTTONS)}
          color="secondary"
          disabled={buttons === DEFAULT_CLIP_BUTTONS}
          css={css`
            margin-right: auto;
          `}
        >
          {Messages.hotkeyReset()}
        </Button>
        <Button onClick={onClose} color="secondary">
          {Messages.hotkeyCancel()}
        </Button>
        <Button
          onClick={() => {
            onApply(buttons);
            onClose();
          }}
          color="primary"
          disabled={buttons === 0 || buttons === current}
        >
          {Messages.hotkeyApply()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
