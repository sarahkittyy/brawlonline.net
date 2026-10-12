import type { StageLists } from "@common/rulesets";
import { PPLUS_LEGAL_STAGES, stageName } from "@common/rulesets";
import { PPLUS_STAGES } from "@common/stages";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
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

const StageTile = ({
  id,
  role,
  icon,
  onRole,
}: {
  id: number;
  role: StageRole;
  icon?: string;
  onRole: (role: StageRole) => void;
}) => (
  <div className={clsx(styles.tile, styles[role])} data-stage={id} data-role={role}>
    <div className={styles.banner}>
      {icon && <img className={styles.art} src={icon} alt="" draggable={false} />}
      <span className={styles.name}>{stageName(id)}</span>
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
}: {
  stages: StageLists;
  onRole: (id: number, role: StageRole) => void;
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
  const [open, setOpen] = useState(otherOn > 0);

  const tiles = (ids: number[]) => (
    <div className={styles.grid}>
      {ids.map((id) => (
        <StageTile
          key={id}
          id={id}
          role={stageRole(stages, id)}
          icon={iconOf(id)}
          onRole={(role) => onRole(id, role)}
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
