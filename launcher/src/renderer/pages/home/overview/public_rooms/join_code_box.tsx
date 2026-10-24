import { isRoomCode } from "@dolphin/hand_off_target";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import { useState } from "react";

import { PublicRoomsMessages as Messages } from "./public_rooms.messages";
import styles from "./public_rooms.module.css";

export type JoinCodeBoxProps = {
  /** Joins the room; resolves to whether the game took the code (the box is then cleared). */
  onJoin: (code: string) => Promise<boolean> | void;
  /** This box's join is being handed to the game. */
  joining: boolean;
  /** Another join or a create is in progress. */
  disabled: boolean;
};

/**
 * Home's room code box: the 4 letters of any room, public or private (as in game, WITH FRIENDS > Join Room), and
 * Join. Letters only, uppercase; four that aren't a room code (a vowel, a Y) turn the box red.
 */
export const JoinCodeBox = ({ onJoin, joining, disabled }: JoinCodeBoxProps) => {
  const [text, setText] = useState("");
  const valid = isRoomCode(text);
  const typo = text.length === 4 && !valid;

  const join = () => {
    if (!valid || disabled || joining) {
      return;
    }
    void Promise.resolve(onJoin(text)).then((taken) => {
      if (taken) {
        setText("");
      }
    });
  };

  return (
    <div className={styles.joinCode}>
      <Tooltip
        title={typo ? Messages.notARoomCode() : Messages.roomCodeTooltip()}
        placement="top"
        enterDelay={typo ? 0 : 600}
        disableInteractive={true}
      >
        <TextField
          size="small"
          hiddenLabel={true}
          value={text}
          placeholder={Messages.roomCodePlaceholder()}
          error={typo}
          inputProps={{
            maxLength: 4,
            spellCheck: false,
            autoComplete: "off",
            "aria-label": Messages.roomCodeLabel(),
            "data-testid": "room-code",
          }}
          onChange={(e) =>
            setText(
              e.target.value
                .toUpperCase()
                .replace(/[^A-Z]/g, "")
                .slice(0, 4),
            )
          }
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              join();
            }
          }}
          sx={{
            width: 112,
            "& input": {
              fontFamily: "monospace",
              letterSpacing: "0.15em",
              py: "5px",
            },
            // As the ruleset box: the placeholder plain and smaller, the box's height from the input's own font.
            "& input::placeholder": {
              fontFamily: (theme) => theme.typography.fontFamily,
              fontSize: "0.75rem",
              letterSpacing: "normal",
            },
          }}
        />
      </Tooltip>
      <Button
        variant="outlined"
        color="secondary"
        size="small"
        disabled={!valid || disabled || joining}
        onClick={join}
        data-testid="join-code"
        startIcon={joining ? <CircularProgress color="inherit" size={14} /> : undefined}
        sx={{ textTransform: "none", fontWeight: 600, borderRadius: "8px", px: 1.5, whiteSpace: "nowrap" }}
      >
        {Messages.joinCode()}
      </Button>
    </div>
  );
};
