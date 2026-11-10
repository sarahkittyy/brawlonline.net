// What Project Punch's popup and Settings > Training offer for a state (dolphin/project_punch).
import type { ProjectPunchState } from "@dolphin/project_punch/types";

/** What can be done in `state` (the popup's and Settings > Training's buttons). */
export function projectPunchActions(state: ProjectPunchState | null) {
  const installed = state?.installed ?? null;
  const job = state?.job ?? null;
  const canPlay = installed != null && state != null && !state.needsCard && job == null;
  const canGet = state != null && !state.upToDate && (state.latest != null || installed == null);
  return {
    installed,
    job,
    canPlay,
    /** Download (nothing installed) or Update; null when there is nothing to get. */
    get: canGet && state?.latest ? (installed ? ("update" as const) : ("download" as const)) : null,
    /** GitHub could not be reached and nothing is installed. */
    offline: state != null && state.latest == null && installed == null,
  };
}

export const jobPercent = (job: ProjectPunchState["job"]) =>
  job && job.total > 0 ? Math.min(100, (job.current / job.total) * 100) : 0;
