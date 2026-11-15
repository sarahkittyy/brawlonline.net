import type { DolphinSetupPhase } from "@dolphin/types";
import DownloadRoundedIcon from "@mui/icons-material/DownloadRounded";
import ButtonBase from "@mui/material/ButtonBase";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";

import { type LauncherUpdateAction } from "@/app/header/play_button/launcher_update";
import { setupPhaseLabel } from "@/app/header/play_button/play_button";
import { PlayButtonMessages } from "@/app/header/play_button/play_button.messages";

import styles from "./update_ring.module.css";

const percentOf = (fraction: number) => Math.floor(Math.min(1, Math.max(0, fraction)) * 100);

/**
 * The folded sidebar's launcher update (docs/design/launcher-redesign.md §14): the open sidebar's Update button as a
 * ring that fills as the update downloads. Its tooltip has the button's words ("Update 45%", "v0.2.1 → v0.2.2"); a
 * click does what the button does. Downloaded (or to fetch from the website), the whole circle is the accent.
 */
export const LauncherUpdateRing = ({ update }: { update: LauncherUpdateAction }) => {
  const { downloadProgress, currentVersion, newVersion, onClick, disabled, title } = update;
  const downloading = downloadProgress !== undefined;
  const percent = percentOf(downloadProgress ?? 0);
  const tip = (
    <span className={styles.tip}>
      <span>{downloading ? `${PlayButtonMessages.update()} ${percent}%` : PlayButtonMessages.update()}</span>
      <span>{`v${currentVersion} → v${newVersion}`}</span>
      {title && <span>{title}</span>}
    </span>
  );
  return (
    <Tooltip title={tip} placement="right" disableInteractive={true}>
      {/* A disabled button gets no hover: the span keeps the tooltip while the update downloads or installs. */}
      <span>
        <ButtonBase
          className={clsx(styles.ring, !downloading && !disabled && styles.ready)}
          onClick={onClick}
          disabled={downloading || disabled}
          aria-label={`${PlayButtonMessages.update()} v${currentVersion} → v${newVersion}`}
          data-testid="update-ring"
        >
          {downloading && <Ring value={percent} />}
          {disabled && <CircularProgress className={styles.fill} size={36} thickness={3} />}
          <DownloadRoundedIcon className={styles.icon} />
        </ButtonBase>
      </span>
    </Tooltip>
  );
};

/** A set-up step (installing Dolphin, downloading Project+...) as a ring: its name and percentage in the tooltip. */
export const SetupRing = ({ phase, fillPercent }: { phase: DolphinSetupPhase | undefined; fillPercent: number }) => {
  const percent = percentOf(fillPercent);
  const label = phase ? `${setupPhaseLabel(phase)} ${percent}%` : PlayButtonMessages.updating();
  return (
    <Tooltip title={label} placement="right" disableInteractive={true}>
      <span className={styles.ring} role="progressbar" aria-label={label} data-testid="setup-ring">
        {phase ? <Ring value={percent} /> : <CircularProgress className={styles.fill} size={36} thickness={3} />}
        <DownloadRoundedIcon className={styles.icon} />
      </span>
    </Tooltip>
  );
};

const Ring = ({ value }: { value: number }) => (
  <>
    <CircularProgress className={styles.track} variant="determinate" value={100} size={36} thickness={3} />
    <CircularProgress className={styles.fill} variant="determinate" value={value} size={36} thickness={3} />
  </>
);
