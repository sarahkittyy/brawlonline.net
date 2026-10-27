import type { MatchRules, SpecialRules } from "@common/rulesets";
import { SPECIAL_ROWS, specialOn, STAMINA_TYPES } from "@common/rulesets";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Typography from "@mui/material/Typography";

import styles from "./rulesets_page.module.css";
import { SpecialMessages as Messages } from "./special_section.messages";

const toggleSx = { textTransform: "none", py: "2px", px: 1.25 } as const;

/** "Stamina, Turbo" for the rows that are on, or "Off". */
export function specialLine(special: SpecialRules | undefined): string {
  const on = specialOn(special);
  const values = SPECIAL_ROWS.filter((r) => on[r.key]).map((r) => Messages.value(on[r.key] ?? ""));
  return values.length ? values.join(", ") : Messages.off();
}

/**
 * The rules' Special Versus modes (docs/design/special-rules.md): a row per row of P+'s box, OFF or one of its two
 * values, and the stamina type with stamina. Without `onChange` it is read-only.
 */
export const SpecialSection = ({
  rules,
  onChange,
}: {
  rules: MatchRules;
  onChange?: (patch: Partial<MatchRules>) => void;
}) => {
  const on = specialOn(rules.special);
  const setRow = (key: string, value: string) => {
    const special: SpecialRules = { ...on };
    if (value === "off") {
      delete special[key as keyof SpecialRules];
    } else {
      special[key as keyof SpecialRules] = value;
    }
    onChange?.({ special, staminaType: special.stamina === "stamina" ? rules.staminaType ?? 1 : 1 });
  };
  return (
    <div data-testid="special-section">
      <Typography variant="caption" color="text.secondary">
        {Messages.intro()}
      </Typography>
      {SPECIAL_ROWS.map((row) => {
        const value = on[row.key] ?? "off";
        return (
          <div key={row.key} className={styles.row} data-special={row.key}>
            <div className={styles.rowLabel}>
              <span>{Messages.row(row.key)}</span>
              {value !== "off" && <span className={styles.rowHelp}>{Messages.help(value)}</span>}
            </div>
            <div className={styles.rowControl}>
              {onChange ? (
                <ToggleButtonGroup
                  size="small"
                  exclusive={true}
                  value={value}
                  onChange={(_, v: string | null) => v !== null && setRow(row.key, v)}
                  aria-label={Messages.row(row.key)}
                >
                  {["off", ...row.values].map((v) => (
                    <ToggleButton key={v} value={v} sx={toggleSx}>
                      {v === "off" ? Messages.off() : Messages.value(v)}
                    </ToggleButton>
                  ))}
                </ToggleButtonGroup>
              ) : (
                <span>{value === "off" ? Messages.off() : Messages.value(value)}</span>
              )}
            </div>
          </div>
        );
      })}
      {on.stamina === "stamina" && (
        <div className={styles.row}>
          <div className={styles.rowLabel}>
            <span>{Messages.staminaType()}</span>
            <span className={styles.rowHelp}>{Messages.staminaTypeHelp()}</span>
          </div>
          <div className={styles.rowControl}>
            {onChange ? (
              <ToggleButtonGroup
                size="small"
                exclusive={true}
                value={String(rules.staminaType ?? 1)}
                onChange={(_, v: string | null) => v !== null && onChange({ staminaType: Number(v) })}
                aria-label={Messages.staminaType()}
              >
                {Array.from({ length: STAMINA_TYPES }, (_, i) => String(i + 1)).map((v) => (
                  <ToggleButton key={v} value={v} sx={toggleSx}>
                    {v}
                  </ToggleButton>
                ))}
              </ToggleButtonGroup>
            ) : (
              <span>{rules.staminaType ?? 1}</span>
            )}
          </div>
        </div>
      )}
    </div>
  );
};
