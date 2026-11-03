import type { PadReaderStatus } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import Typography from "@mui/material/Typography";
import log from "electron-log";
import React from "react";

import { GcButtonRow } from "@/components/gc_button_icons/gc_button_icons";
import { useServices } from "@/services";

import { ClipsSettingsMessages as Messages } from "./clips_settings.messages";

type Phase = "waiting" | "entering" | "done";

/** What to tell the player about their controllers, or null when one is there to press. */
function statusMessage(status: PadReaderStatus | null, error: string | null): string | null {
  if (error !== null) {
    return Messages.hotkeyReadError(error);
  }
  if (status === null) {
    return Messages.hotkeyLooking();
  }
  if (status.ports.length === 0) {
    return Messages.hotkeyNoControllers();
  }
  if (status.ports.some((p) => p.plugged)) {
    return null;
  }
  if (status.adapter === "error") {
    return Messages.hotkeyAdapterError(status.adapterError ?? "");
  }
  return Messages.hotkeyNotPlugged();
}

/**
 * Configure for Manual Clip Hotkey: "Press any combination of buttons." Every button pressed
 * joins the hotkey until all are let go, which finishes it; then Retry starts over and Apply keeps
 * it. The buttons are the controllers' as Dolphin reads them (dolphin/pad_reader.ts).
 */
export const ClipHotkeyDialog = ({
  open,
  onClose,
  onApply,
}: {
  open: boolean;
  onClose: () => void;
  onApply: (buttons: number) => void;
}) => {
  const { dolphinService } = useServices();
  const [phase, setPhase] = React.useState<Phase>("waiting");
  const [captured, setCaptured] = React.useState(0);
  const [status, setStatus] = React.useState<PadReaderStatus | null>(null);
  const [error, setError] = React.useState<string | null>(null);
  const phaseRef = React.useRef<Phase>("waiting");
  const capturedRef = React.useRef(0);

  const setBoth = (p: Phase, buttons: number) => {
    phaseRef.current = p;
    capturedRef.current = buttons;
    setPhase(p);
    setCaptured(buttons);
  };

  React.useEffect(() => {
    if (!open) {
      return;
    }
    const readerId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    setBoth("waiting", 0);
    setStatus(null);
    setError(null);
    // Buttons count once the controllers have been seen with none held (not one held while opening).
    let armed = false;
    const unsubscribe = dolphinService.onEvent(DolphinEventType.PAD_READER, (event) => {
      if (event.readerId !== readerId) {
        return;
      }
      if (event.status) {
        setStatus(event.status);
      }
      if (event.ended) {
        if (event.ended.error !== null) {
          setError(event.ended.error);
        }
        return;
      }
      if (event.buttons === undefined) {
        return;
      }
      if (!armed) {
        armed = event.buttons === 0;
        return;
      }
      if (phaseRef.current === "done") {
        return;
      }
      if (event.buttons !== 0) {
        setBoth("entering", capturedRef.current | event.buttons);
      } else if (capturedRef.current !== 0) {
        setBoth("done", capturedRef.current);
      }
    });
    dolphinService.startPadReader(readerId).catch((err) => {
      log.error(err);
      setError(err instanceof Error ? err.message : String(err));
    });
    return () => {
      unsubscribe();
      void dolphinService.stopPadReader(readerId).catch(log.error);
    };
  }, [open, dolphinService]);

  const message = statusMessage(status, error);
  const done = phase === "done";

  return (
    <Dialog open={open} onClose={onClose} maxWidth="xs" fullWidth={true}>
      <DialogContent
        css={css`
          display: flex;
          flex-direction: column;
          align-items: center;
          gap: 16px;
          padding-top: 24px;
        `}
      >
        <Typography variant="subtitle1">{Messages.hotkeyPressButtons()}</Typography>
        <div
          css={css`
            min-height: 3.5rem;
            display: flex;
            align-items: center;
            justify-content: center;
          `}
        >
          {captured !== 0 ? (
            <GcButtonRow buttons={captured} size={1.2} />
          ) : (
            <Typography
              variant="body2"
              css={css`
                opacity: 0.6;
              `}
            >
              {message ?? Messages.hotkeyWaiting()}
            </Typography>
          )}
        </div>
        {captured !== 0 && !done && <Typography variant="caption">{Messages.hotkeyLetGo()}</Typography>}
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} color="secondary">
          {Messages.hotkeyCancel()}
        </Button>
        <Button onClick={() => setBoth("waiting", 0)} color="secondary" disabled={!done}>
          {Messages.hotkeyRetry()}
        </Button>
        <Button
          onClick={() => {
            onApply(capturedRef.current);
            onClose();
          }}
          color="primary"
          disabled={!done}
        >
          {Messages.hotkeyApply()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
