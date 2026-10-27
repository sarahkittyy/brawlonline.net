import type { StageLists } from "@common/rulesets";
import { MAX_HAZARD_STAGES, PPLUS_LEGAL_STAGES, stageName } from "@common/rulesets";
import { PPLUS_STAGES } from "@common/stages";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import WarningAmberRoundedIcon from "@mui/icons-material/WarningAmberRounded";
import WarningRoundedIcon from "@mui/icons-material/WarningRounded";
import { clsx } from "clsx";
import { useState } from "react";

import { assetUrl, useGameAssets } from "@/styles/game_theme";

import { StagesMessages as Messages } from "./stages_section.messages";
import styles from "./stages_section.module.css";

export type StageRole = "starter" | "counterpick" | "off";

const ROLES: StageRole[] = ["starter", "counterpick", "off"];
const ROLE_LABEL: Record<StageRole, () => string> = {
  starter: Messages.starter,
  counterpick: Messages.counterpick,
  off: Messages.off,
};

export function stageRole(stages: StageLists, id: number): StageRole {
  return stages.starters.includes(id) ? "starter" : stages.counterpicks.includes(id) ? "counterpick" : "off";
}

/** The stage's icon from the user's P+ files (src/brawl_assets/stages.ts), or undefined before they are extracted. */
export const useStageIcon = () => {
  const icons = useGameAssets((s) => s.state?.manifest?.stages);
  return (id: number): string | undefined => {
    const e = icons?.[String(id)];
    return e ? assetUrl(e.file) : undefined;
  };
};

/**
 * Per-stage hazards: the rules' default, and the stages that go the other way (`hazardStages`). The editor passes it;
 * without it the tiles show no hazards.
 */
export type StageHazards = {
  default: boolean;
  stages: number[];
  onToggle: (id: number) => void;
};

/**
 * A tile's hazard button: a small warning sign cut into the banner's top right corner (borders only on its left and
 * bottom). The sign is the stage's hazards: a yellow triangle on, a grey outline crossed out off. A stage with its own
 * hazards always shows it; one that follows the default only while the tile is hovered, so the grid stays as clean as
 * it was with one switch for every stage.
 */
const HazardButton = ({ id, hazards }: { id: number; hazards: StageHazards }) => {
  const own = hazards.stages.includes(id);
  const on = hazards.default !== own;
  const full = !own && hazards.stages.length >= MAX_HAZARD_STAGES;
  const title = full
    ? Messages.hazardsFull(MAX_HAZARD_STAGES)
    : own
    ? Messages.hazardsOwnTitle(on)
    : Messages.hazardsDefaultTitle(on);
  return (
    <button
      type="button"
      className={clsx(styles.hazard, !own && styles.hazardDefault, on && styles.hazardOn)}
      title={title}
      aria-label={title}
      aria-pressed={own}
      disabled={full}
      data-hazards={on ? "on" : "off"}
      data-hazards-own={own}
      onClick={() => hazards.onToggle(id)}
    >
      {on ? (
        <WarningRoundedIcon className={styles.hazardIcon} />
      ) : (
        <WarningAmberRoundedIcon className={styles.hazardIcon} />
      )}
    </button>
  );
};

const StageTile = ({
  id,
  role,
  icon,
  onRole,
  hazards,
}: {
  id: number;
  role: StageRole;
  icon?: string;
  onRole: (role: StageRole) => void;
  hazards?: StageHazards;
}) => (
  <div className={clsx(styles.tile, styles[role])} data-stage={id} data-role={role}>
    <div className={styles.banner}>
      {icon && <img className={styles.art} src={icon} alt="" draggable={false} />}
      <span className={styles.name}>{stageName(id)}</span>
      {hazards && <HazardButton id={id} hazards={hazards} />}
    </div>
    <div className={styles.roles} role="group" aria-label={stageName(id)}>
      {ROLES.map((r) => (
        <button
          key={r}
          type="button"
          className={clsx(styles.role, r === role && styles.roleOn)}
          aria-pressed={r === role}
          data-role-button={r}
          onClick={() => r !== role && onRole(r)}
        >
          {ROLE_LABEL[r]()}
        </button>
      ))}
    </div>
  </div>
);

/**
 * The ruleset's stages as tiles: each stage's name on its stage select icon, and Starter / Counterpick / Off under
 * it. P+'s legal stages first; the rest of P+'s stage select (and any other id a file brought) under "Other stages",
 * collapsed unless one of them is on.
 */
export const StagesSection = ({
  stages,
  onRole,
  icon,
  hazards,
}: {
  stages: StageLists;
  onRole: (id: number, role: StageRole) => void;
  /** Per-stage hazards (the editor's); none: the tiles show no hazards. */
  hazards?: StageHazards;
  /** The stage's icon URL; the extracted P+ icons by default. */
  icon?: (id: number) => string | undefined;
}) => {
  const extracted = useStageIcon();
  const iconOf = icon ?? extracted;
  const legalIds = PPLUS_LEGAL_STAGES.map((s) => s.id);
  const listed = [...stages.starters, ...stages.counterpicks];
  const otherIds = [
    ...PPLUS_STAGES.map((s) => s.id).filter((id) => !legalIds.includes(id)),
    ...listed.filter((id) => !legalIds.includes(id) && !PPLUS_STAGES.some((s) => s.id === id)),
  ];
  const otherOn = otherIds.filter((id) => listed.includes(id)).length;
  const otherOwnHazards = otherIds.some((id) => hazards?.stages.includes(id));
  const [open, setOpen] = useState(otherOn > 0 || otherOwnHazards);

  const tiles = (ids: number[]) => (
    <div className={styles.grid}>
      {ids.map((id) => (
        <StageTile
          key={id}
          id={id}
          role={stageRole(stages, id)}
          icon={iconOf(id)}
          onRole={(role) => onRole(id, role)}
          hazards={hazards}
        />
      ))}
    </div>
  );

  return (
    <div className={styles.section} data-testid="stages-section">
      {tiles(legalIds)}
      <div className={styles.other}>
        <button
          type="button"
          className={styles.otherHeader}
          aria-expanded={open}
          onClick={() => setOpen(!open)}
          data-testid="other-stages-toggle"
        >
          <span className={styles.otherTitle}>{Messages.otherStages()}</span>
          <span className={styles.otherSummary}>{Messages.otherOn(otherOn)}</span>
          <ExpandMoreIcon className={clsx(styles.chevron, open && styles.chevronOpen)} fontSize="small" />
        </button>
        {open && <div className={styles.otherBody}>{tiles(otherIds)}</div>}
      </div>
    </div>
  );
};
