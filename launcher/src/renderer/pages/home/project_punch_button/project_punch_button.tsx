// Home's lab button, at the far right of the tab row: opens Project Punch's popup. While Project
// Punch downloads (the popup may be closed), a thin bar under it shows how far along it is.
import type { ProjectPunchState } from "@dolphin/project_punch/types";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Tooltip from "@mui/material/Tooltip";
import React, { useEffect } from "react";

import { LabIcon } from "@/lib/project_punch/lab_icon";
import { ProjectPunchMessages as Messages } from "@/lib/project_punch/project_punch.messages";
import { jobPercent } from "@/lib/project_punch/project_punch_actions";
import { openProjectPunchDialog, useProjectPunch, watchProjectPunch } from "@/lib/project_punch/use_project_punch";

export type ProjectPunchButtonViewProps = {
  job: ProjectPunchState["job"];
  onClick: () => void;
};

export const ProjectPunchButtonView = ({ job, onClick }: ProjectPunchButtonViewProps) => (
  // Styled like the header's icon buttons (Settings, Discord: 24 px icon, half opacity), centred under
  // the Settings icon: that one's centre is 34 px from the window's right edge (its 10 px margin and
  // half its 48 px), so this 40 px button keeps 14 px to the right; vertically centred on the Play
  // buttons (their 8 px top padding and half their 31 px).
  <div
    style={{
      marginLeft: "auto",
      marginRight: 14,
      paddingTop: 4,
      display: "flex",
      flexDirection: "column",
      alignItems: "center",
    }}
  >
    {/* The button is 14 px from the window's right edge and the tooltip is wider: keep it 8 px in. */}
    <Tooltip
      title={Messages.buttonTooltip()}
      PopperProps={{ modifiers: [{ name: "preventOverflow", options: { padding: 8 } }] }}
    >
      <IconButton
        onClick={onClick}
        aria-label={Messages.buttonTooltip()}
        data-testid="project-punch-button"
        sx={{ width: 40, height: 40, padding: "8px", color: "var(--off-white)", opacity: 0.5 }}
      >
        <LabIcon />
      </IconButton>
    </Tooltip>
    {/* Reserve the space for the bar. A hidden indeterminate bar still runs its animations. */}
    <div style={{ height: 2, marginTop: 1 }}>
      {job && (
        <LinearProgress
          data-testid="project-punch-progress"
          variant={job.total > 0 ? "determinate" : "indeterminate"}
          value={jobPercent(job)}
          color="secondary"
          sx={{ width: 24, height: 2, borderRadius: 1 }}
        />
      )}
    </div>
  </div>
);

export const ProjectPunchButton = React.memo(function ProjectPunchButton() {
  const job = useProjectPunch((s) => s.state?.job ?? null);
  useEffect(() => watchProjectPunch(), []);
  return <ProjectPunchButtonView job={job} onClick={openProjectPunchDialog} />;
});
