import type { Ruleset } from "@common/rulesets";
import { picksUsed, SPECIAL_ROWS, specialOn, stageName } from "@common/rulesets";
import Typography from "@mui/material/Typography";

import { ItemsSection } from "./items_section";
import { RulesetDetailsMessages as Messages } from "./rulesets_page.messages";
import styles from "./rulesets_page.module.css";
import { SpecialMessages } from "./special_section.messages";

const onOff = (b: boolean) => (b ? Messages.on() : Messages.off());

function formatRows(r: Ruleset): [string, string][] {
  const rows: [string, string][] = [];
  if (r.bestOf > 0) {
    rows.push([Messages.set(), Messages.bestOf(r.bestOf)]);
    rows.push([Messages.game1(), Messages.strikes(r.strikes.join("-"))]);
    rows.push([Messages.games2(), Messages.bansThenPick(r.bans)]);
    rows.push([Messages.dsr(), onOff(r.dsr)]);
  } else {
    rows.push([Messages.set(), Messages.noSet()]);
    const choice =
      r.stageChoice === "strike"
        ? Messages.strikeEveryGame(r.strikes.join("-"))
        : r.stageChoice === "random"
        ? Messages.randomEveryGame()
        : r.stageChoice === "anyone-picks"
        ? Messages.anyonePicksEveryGame()
        : r.stageChoice === "loser-picks-list"
        ? Messages.loserPicksList()
        : Messages.loserPicks();
    rows.push([Messages.stages(), choice]);
  }
  if (picksUsed(r)) {
    rows.push([Messages.alternateStages(), r.alternateStages ? Messages.alternateStagesOn() : Messages.off()]);
  }
  return rows;
}

function ruleRows(r: Ruleset): [string, string][] {
  const m = r.rules;
  const style = { stock: Messages.stock(), time: Messages.time(), coin: Messages.coin() }[m.style] ?? m.style;
  const rows: [string, string][] = [[Messages.style(), style]];
  if (m.style === "stock") {
    rows.push([Messages.stocks(), String(m.stocks)]);
  }
  // (as the game plays it: stock matches, or any style with regen's frame control; not with stamina)
  const regen = m.special?.attack === "regen";
  if (m.stockControl && (m.style === "stock" || regen) && m.special?.stamina !== "stamina") {
    rows.push([regen ? Messages.frameControl() : Messages.stockControl(), Messages.stockControlOn()]);
  }
  rows.push(
    [Messages.timeLimit(), m.timeMinutes === 0 ? Messages.noLimit() : Messages.minutes(m.timeMinutes)],
    [Messages.damageRatio(), m.damageRatio.toFixed(1)],
    [Messages.teamAttack(), onOff(m.teamAttack)],
    [Messages.pause(), m.pause === "hold" ? Messages.holdStart() : Messages.on()],
    [
      Messages.hazards(),
      m.hazardStages?.length
        ? Messages.hazardsExcept(m.hazards, m.hazardStages.map(stageName).join(", "))
        : onOff(m.hazards),
    ],
    [Messages.autoLCancel(), onOff(m.autoLCancel)],
    [Messages.inputAssist(), onOff(m.inputAssist)],
  );
  return rows;
}

/** The Special Versus rows that are on (none: no table). */
function specialRows(r: Ruleset): [string, string][] {
  const on = specialOn(r.rules.special);
  const rows: [string, string][] = SPECIAL_ROWS.filter((row) => on[row.key]).map((row) => [
    SpecialMessages.row(row.key),
    SpecialMessages.value(on[row.key] ?? ""),
  ]);
  if (on.stamina === "stamina") {
    const type = r.rules.staminaType ?? 1;
    rows.push([Messages.staminaType(), `${type}: ${SpecialMessages.staminaTypeHelp(type)}`]);
  }
  return rows;
}

const Table = ({ title, rows }: { title?: string; rows: [string, string][] }) => (
  <section className={styles.section}>
    {title && (
      <Typography variant="subtitle2" className={styles.sectionTitle}>
        {title}
      </Typography>
    )}
    <dl className={styles.details}>
      {rows.map(([k, v]) => (
        <div key={k} className={styles.detailRow}>
          <dt>{k}</dt>
          <dd>{v}</dd>
        </div>
      ))}
    </dl>
  </section>
);

const Stages = ({ title, ids }: { title: string; ids: number[] }) => (
  <section className={styles.section}>
    <Typography variant="subtitle2" className={styles.sectionTitle}>
      {title} ({ids.length})
    </Typography>
    <div className={styles.stageChips}>
      {ids.length === 0 ? <span className={styles.itemMeta}>{Messages.none()}</span> : null}
      {ids.map((id) => (
        <span key={id} className={styles.stageChip}>
          {stageName(id)}
        </span>
      ))}
    </div>
  </section>
);

/** A ruleset, read-only: the set format, every rule, the items as P+'s ITEM SWITCH shows them, the stages. */
export const RulesetDetails = ({ ruleset }: { ruleset: Ruleset }) => {
  const r = ruleset;
  return (
    <div className={styles.editorFields} data-testid="ruleset-details">
      <Table rows={formatRows(r)} />
      <Table title={Messages.rules()} rows={ruleRows(r)} />
      {specialRows(r).length > 0 && <Table title={Messages.special()} rows={specialRows(r)} />}
      <ItemsSection frequency={r.rules.itemFrequency} itemSwitch={r.rules.itemSwitch} />
      <Stages
        title={r.bestOf > 0 || r.stageChoice === "strike" ? Messages.starters() : Messages.stageList()}
        ids={r.stages.starters}
      />
      {(r.stages.counterpicks.length > 0 || r.bestOf > 0) && (
        <Stages title={Messages.counterpicks()} ids={r.stages.counterpicks} />
      )}
    </div>
  );
};
