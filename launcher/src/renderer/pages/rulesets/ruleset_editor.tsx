import type { MatchRules, Ruleset, RulesetPlayers, StageChoice } from "@common/rulesets";
import {
  MAX_BANS,
  MAX_RULESET_NAME_LEN,
  PPLUS_LEGAL_STAGES,
  stageName,
  strikesUsed,
  suggestStrikes,
} from "@common/rulesets";
import Button from "@mui/material/Button";
import MenuItem from "@mui/material/MenuItem";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Typography from "@mui/material/Typography";
import React, { useEffect, useState } from "react";

import { ItemsSection } from "./items_section";
import { RulesetEditorMessages as Messages } from "./rulesets_page.messages";
import styles from "./rulesets_page.module.css";

type StageRole = "starter" | "counterpick" | "off";

/** Every field of a ruleset (docs/rulesets-protocol.md §1), controlled. */
export const RulesetEditor = ({ value, onChange }: { value: Ruleset; onChange: (r: Ruleset) => void }) => {
  const r = value;
  const set = (patch: Partial<Ruleset>) => onChange({ ...r, ...patch });
  const setRules = (patch: Partial<MatchRules>) => onChange({ ...r, rules: { ...r.rules, ...patch } });

  const setPlayers = (players: RulesetPlayers) => {
    const next: Ruleset = { ...r, players };
    if (players === "ffa") {
      next.bestOf = 0;
      if (next.stageChoice === "strike") {
        next.stageChoice = "loser-picks";
      }
    }
    onChange(withStrikes(next));
  };
  const setBestOf = (bestOf: number) => {
    const next: Ruleset = { ...r, bestOf };
    if (r.bestOf === 0 && bestOf > 0) {
      // A set: Ranked's defaults to start from.
      next.bans = 2;
      next.dsr = true;
    }
    onChange(withStrikes(next));
  };
  const setStageRole = (id: number, role: StageRole) => {
    const starters = r.stages.starters.filter((s) => s !== id);
    const counterpicks = r.stages.counterpicks.filter((s) => s !== id);
    if (role === "starter") {
      starters.push(id);
    } else if (role === "counterpick") {
      counterpicks.push(id);
    }
    const byId = (a: number, b: number) => a - b;
    onChange(withStrikes({ ...r, stages: { starters: starters.sort(byId), counterpicks: counterpicks.sort(byId) } }));
  };

  // P+'s legal stages, then any other id an imported file brought.
  const legalIds = PPLUS_LEGAL_STAGES.map((s) => s.id);
  const otherIds = [...r.stages.starters, ...r.stages.counterpicks].filter((id) => !legalIds.includes(id));
  const roleOf = (id: number): StageRole =>
    r.stages.starters.includes(id) ? "starter" : r.stages.counterpicks.includes(id) ? "counterpick" : "off";

  return (
    <div className={styles.editorFields}>
      <TextField
        label={Messages.name()}
        value={r.name}
        onChange={(e) => set({ name: e.target.value })}
        helperText={Messages.nameHelp(MAX_RULESET_NAME_LEN)}
        inputProps={{ maxLength: MAX_RULESET_NAME_LEN + 8, "data-testid": "ruleset-name-input" }}
        size="small"
        fullWidth={true}
      />

      <Section title={Messages.format()}>
        <Row label={Messages.players()}>
          <Choice
            value={r.players}
            onChange={setPlayers}
            options={[
              ["1v1", Messages.oneVsOne()],
              ["2v2", Messages.twoVsTwo()],
              ["ffa", Messages.ffa()],
            ]}
          />
        </Row>
        <Row label={Messages.set()}>
          <Choice
            value={String(r.bestOf)}
            onChange={(v) => setBestOf(Number(v))}
            options={[
              ["0", Messages.noSet()],
              ["3", Messages.bestOf(3), r.players === "ffa"],
              ["5", Messages.bestOf(5), r.players === "ffa"],
              ["7", Messages.bestOf(7), r.players === "ffa"],
            ]}
          />
        </Row>
        {r.bestOf === 0 ? (
          <Row label={Messages.stageChoice()} help={stageChoiceHelp(r.stageChoice)}>
            <Choice
              value={r.stageChoice}
              onChange={(stageChoice: StageChoice) => onChange(withStrikes({ ...r, stageChoice }))}
              options={[
                ["loser-picks", Messages.loserPicks()],
                ["strike", Messages.strike(), r.players === "ffa"],
                ["random", Messages.random()],
              ]}
            />
          </Row>
        ) : (
          <Typography variant="caption" color="text.secondary">
            {Messages.setHelp()}
          </Typography>
        )}
        {r.players === "ffa" && (
          <Typography variant="caption" color="text.secondary">
            {Messages.ffaHelp()}
          </Typography>
        )}
        {strikesUsed(r) && (
          <Row label={Messages.strikes()} help={Messages.strikesHelp(r.stages.starters.length)}>
            <StrikesInput value={r.strikes} onChange={(strikes) => set({ strikes })} />
            <Button
              size="small"
              sx={{ textTransform: "none" }}
              onClick={() => set({ strikes: suggestStrikes(r.stages.starters.length) })}
            >
              {Messages.suggest()}
            </Button>
          </Row>
        )}
        {r.bestOf > 0 && (
          <>
            <Row label={Messages.bans()}>
              <Choice
                value={String(r.bans)}
                onChange={(v) => set({ bans: Number(v) })}
                options={Array.from({ length: MAX_BANS + 1 }, (_, n) => [String(n), String(n)] as [string, string])}
              />
            </Row>
            <Row label={Messages.dsr()} help={Messages.dsrHelp()}>
              <Switch checked={r.dsr} onChange={(e) => set({ dsr: e.target.checked })} />
            </Row>
          </>
        )}
      </Section>

      <Section title={Messages.rules()}>
        <Row label={Messages.style()}>
          <Choice
            value={r.rules.style}
            onChange={(style: MatchRules["style"]) =>
              setRules({ style, timeMinutes: style !== "stock" && r.rules.timeMinutes === 0 ? 2 : r.rules.timeMinutes })
            }
            options={[
              ["stock", Messages.stock()],
              ["time", Messages.time()],
              ["coin", Messages.coin()],
            ]}
          />
        </Row>
        <Row label={Messages.stocks()}>
          <NumberInput value={r.rules.stocks} min={1} max={99} onChange={(stocks) => setRules({ stocks })} />
        </Row>
        <Row label={Messages.timeLimit()} help={r.rules.style === "stock" ? Messages.timeLimitHelp() : undefined}>
          <NumberInput
            value={r.rules.timeMinutes}
            min={r.rules.style === "stock" ? 0 : 1}
            max={99}
            onChange={(timeMinutes) => setRules({ timeMinutes })}
          />
        </Row>
        <Row label={Messages.damageRatio()}>
          <TextField
            select={true}
            size="small"
            hiddenLabel={true}
            value={Math.round(r.rules.damageRatio * 10)}
            onChange={(e) => setRules({ damageRatio: Number(e.target.value) / 10 })}
            sx={{ width: 100 }}
          >
            {Array.from({ length: 16 }, (_, i) => i + 5).map((t) => (
              <MenuItem key={t} value={t}>
                {(t / 10).toFixed(1)}
              </MenuItem>
            ))}
          </TextField>
        </Row>
        <Row label={Messages.teamAttack()}>
          <Switch checked={r.rules.teamAttack} onChange={(e) => setRules({ teamAttack: e.target.checked })} />
        </Row>
        <Row label={Messages.pause()}>
          <Choice
            value={r.rules.pause}
            onChange={(pause: MatchRules["pause"]) => setRules({ pause })}
            options={[
              ["normal", Messages.pauseNormal()],
              ["hold", Messages.pauseHold()],
            ]}
          />
        </Row>
        <Row label={Messages.autoLCancel()}>
          <Switch checked={r.rules.autoLCancel} onChange={(e) => setRules({ autoLCancel: e.target.checked })} />
        </Row>
        <Row label={Messages.inputAssist()}>
          <Switch checked={r.rules.inputAssist} onChange={(e) => setRules({ inputAssist: e.target.checked })} />
        </Row>
      </Section>

      <ItemsSection
        frequency={r.rules.itemFrequency}
        itemSwitch={r.rules.itemSwitch}
        onChange={(itemFrequency, itemSwitch) => setRules({ itemFrequency, itemSwitch })}
      />

      <Section title={Messages.stages()}>
        <Typography variant="caption" color="text.secondary">
          {Messages.stagesHelp()} {Messages.stageCount(r.stages.starters.length, r.stages.counterpicks.length)}
        </Typography>
        <Row label={Messages.hazards()}>
          <Switch checked={r.rules.hazards} onChange={(e) => setRules({ hazards: e.target.checked })} />
        </Row>
        <div className={styles.stageList}>
          {[...legalIds, ...otherIds].map((id) => (
            <div key={id} className={styles.stageRow} data-stage={id}>
              <span>{stageName(id)}</span>
              <ToggleButtonGroup
                size="small"
                exclusive={true}
                value={roleOf(id)}
                onChange={(_, role: StageRole | null) => role && setStageRole(id, role)}
              >
                <ToggleButton value="starter" sx={toggleSx}>
                  {Messages.stageStarter()}
                </ToggleButton>
                <ToggleButton value="counterpick" sx={toggleSx}>
                  {Messages.stageCounterpick()}
                </ToggleButton>
                <ToggleButton value="off" sx={toggleSx}>
                  {Messages.stageOff()}
                </ToggleButton>
              </ToggleButtonGroup>
            </div>
          ))}
        </div>
      </Section>
    </div>
  );
};

const toggleSx = { textTransform: "none", py: "2px", px: 1.25 } as const;

/** With striking in use, a pattern that no longer fits the starters becomes the suggested one. */
function withStrikes(r: Ruleset): Ruleset {
  if (!strikesUsed(r)) {
    return r;
  }
  const needed = r.stages.starters.length - 1;
  const total = r.strikes.reduce((a, b) => a + b, 0);
  return total === needed && r.strikes.every((n) => n >= 1)
    ? r
    : { ...r, strikes: suggestStrikes(r.stages.starters.length) };
}

function stageChoiceHelp(c: StageChoice): string {
  switch (c) {
    case "loser-picks":
      return Messages.loserPicksHelp();
    case "strike":
      return Messages.strikeHelp();
    case "random":
      return Messages.randomHelp();
  }
}

const Section = ({ title, children }: { title: string; children: React.ReactNode }) => (
  <section className={styles.section}>
    <Typography variant="subtitle2" className={styles.sectionTitle}>
      {title}
    </Typography>
    {children}
  </section>
);

const Row = ({ label, help, children }: { label: string; help?: string; children: React.ReactNode }) => (
  <div className={styles.row}>
    <div className={styles.rowLabel}>
      <span>{label}</span>
      {help && <span className={styles.rowHelp}>{help}</span>}
    </div>
    <div className={styles.rowControl}>{children}</div>
  </div>
);

/** A row of exclusive toggle buttons; an option's third value disables it. */
function Choice<T extends string>({
  value,
  onChange,
  options,
}: {
  value: T;
  onChange: (v: T) => void;
  options: [T, string, boolean?][];
}) {
  return (
    <ToggleButtonGroup
      size="small"
      exclusive={true}
      value={value}
      onChange={(_, v: T | null) => v !== null && onChange(v)}
    >
      {options.map(([v, label, disabled]) => (
        <ToggleButton key={v} value={v} disabled={disabled} sx={toggleSx}>
          {label}
        </ToggleButton>
      ))}
    </ToggleButtonGroup>
  );
}

/** A whole number; the field may hold anything while typing, the ruleset only gets numbers. */
const NumberInput = ({
  value,
  min,
  max,
  onChange,
}: {
  value: number;
  min: number;
  max: number;
  onChange: (n: number) => void;
}) => {
  const [text, setText] = useState(String(value));
  useEffect(() => setText(String(value)), [value]);
  const n = Number(text);
  const bad = text.trim() === "" || !Number.isInteger(n) || n < min || n > max;
  return (
    <TextField
      size="small"
      hiddenLabel={true}
      value={text}
      error={bad}
      inputProps={{ inputMode: "numeric", style: { width: 56 } }}
      onChange={(e) => {
        setText(e.target.value);
        const v = Number(e.target.value);
        if (e.target.value.trim() !== "" && Number.isInteger(v)) {
          onChange(v);
        }
      }}
      onBlur={() => setText(String(value))}
    />
  );
};

/** The strike pattern as "1-2-1". */
const StrikesInput = ({ value, onChange }: { value: number[]; onChange: (s: number[]) => void }) => {
  const [text, setText] = useState(value.join("-"));
  useEffect(() => setText(value.join("-")), [value]);
  return (
    <TextField
      size="small"
      hiddenLabel={true}
      value={text}
      placeholder="1-2-1"
      inputProps={{ style: { width: 90, fontFamily: "monospace" }, "data-testid": "ruleset-strikes" }}
      onChange={(e) => {
        setText(e.target.value);
        const parts = e.target.value.split(/[^0-9]+/).filter((p) => p !== "");
        onChange(parts.map(Number));
      }}
      onBlur={() => setText(value.join("-"))}
    />
  );
};
