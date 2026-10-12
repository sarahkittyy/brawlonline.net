import type { RulesetView } from "@accounts/types";
import { parseRulesetCode } from "@common/rulesets";
import CloseIcon from "@mui/icons-material/Close";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import React, { useEffect, useState } from "react";

import { rulesetTitle, useRememberedRuleset } from "@/lib/rulesets/use_remembered_ruleset";

import styles from "./public_rooms.module.css";
import { RulesetBoxMessages as Messages } from "./ruleset_box.messages";

export type RulesetBoxViewProps = {
  /** The remembered ruleset; null: P+ default. */
  ruleset: RulesetView | null;
  /** Reading or saving it. */
  busy: boolean;
  /** Why the last code was not taken. */
  error?: string;
  onSubmit: (code: string) => void;
  onClear: () => void;
};

/**
 * Home's ruleset box, next to Create room (room-rules.md decision 10): the code of the ruleset new
 * rooms start with, its name beside it. Six valid letters are taken as soon as they are typed; X
 * goes back to P+ default.
 */
export const RulesetBoxView = ({ ruleset, busy, error, onSubmit, onClear }: RulesetBoxViewProps) => {
  const [text, setText] = useState(ruleset?.code ?? "");
  const [typo, setTypo] = useState(false);
  useEffect(() => {
    setText(ruleset?.code ?? "");
    setTypo(false);
  }, [ruleset?.code]);

  const submit = (value: string) => {
    const code = parseRulesetCode(value);
    if (code) {
      setTypo(false);
      if (code !== ruleset?.code) {
        onSubmit(code);
      }
    } else {
      setTypo(value.trim() !== "");
    }
  };

  const shownError = typo ? Messages.notACode() : error;
  return (
    <div className={styles.rulesetBox}>
      <span
        className={styles.rulesetName}
        data-testid="ruleset-name"
        title={ruleset ? rulesetTitle(ruleset) : undefined}
      >
        {shownError ? (
          <span className={styles.rulesetError}>{shownError}</span>
        ) : ruleset ? (
          rulesetTitle(ruleset)
        ) : (
          <span className={styles.rulesetDefault}>{Messages.pplusDefault()}</span>
        )}
      </span>
      <Tooltip title={Messages.tooltip()} placement="top" enterDelay={600} disableInteractive={true}>
        <TextField
          size="small"
          hiddenLabel={true}
          value={text}
          placeholder={Messages.placeholder()}
          error={Boolean(shownError)}
          inputProps={{
            maxLength: 12,
            spellCheck: false,
            "aria-label": Messages.label(),
            "data-testid": "ruleset-code",
          }}
          onChange={(e) => {
            const value = e.target.value.toUpperCase();
            setText(value);
            setTypo(false);
            if (parseRulesetCode(value)) {
              submit(value);
            }
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              submit(text);
            }
          }}
          onBlur={() => {
            if (text.trim() === "" && ruleset) {
              onClear();
            }
          }}
          sx={{
            width: 150,
            "& input": {
              py: "5px",
            },
            // The code in a wide monospace; the placeholder smaller and plain, so it fits the box.
            "& input:not(:placeholder-shown)": {
              fontFamily: "monospace",
              letterSpacing: "0.1em",
            },
            "& input:placeholder-shown": {
              fontSize: "0.75rem",
            },
          }}
          InputProps={{
            endAdornment: (
              <InputAdornment position="end">
                {busy ? (
                  <CircularProgress color="inherit" size={14} />
                ) : (
                  (ruleset || text) && (
                    <Tooltip title={Messages.clear()} disableInteractive={true}>
                      <IconButton
                        size="small"
                        edge="end"
                        aria-label={Messages.clear()}
                        data-testid="ruleset-clear"
                        onClick={() => {
                          setText("");
                          setTypo(false);
                          if (ruleset) {
                            onClear();
                          }
                        }}
                      >
                        <CloseIcon fontSize="inherit" />
                      </IconButton>
                    </Tooltip>
                  )
                )}
              </InputAdornment>
            ),
          }}
        />
      </Tooltip>
    </div>
  );
};

/** The box, reading and saving the account's remembered ruleset. */
export const RulesetBox = React.memo(function RulesetBox() {
  const { ruleset, isLoading, setRemembered, saving } = useRememberedRuleset();
  const [error, setError] = useState<string>();
  const set = (code: string | null) => {
    setError(undefined);
    setRemembered(code).catch((err: unknown) => {
      const status = (err as { status?: number })?.status;
      setError(status === 404 ? Messages.unknownCode() : err instanceof Error ? err.message : String(err));
    });
  };
  return (
    <RulesetBoxView
      ruleset={ruleset}
      busy={isLoading || saving}
      error={error}
      onSubmit={(code) => set(code)}
      onClear={() => set(null)}
    />
  );
});
