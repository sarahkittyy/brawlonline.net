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
  <div style={{ marginLeft: "auto", paddingTop: 8, display: "flex", flexDirection: "column", alignItems: "center" }}>
    <Tooltip title={Messages.buttonTooltip()} placement="bottom" enterDelay={400} disableInteractive={true}>
      <IconButton
        onClick={onClick}
        aria-label={Messages.buttonTooltip()}
        data-testid="project-punch-button"
        sx={{
          width: 38,
          height: 31,
          borderRadius: "8px",
          border: "1px solid rgba(255, 255, 255, 0.14)",
          color: "text.secondary",
          "&:hover": { color: "text.primary", borderColor: "rgba(255, 255, 255, 0.3)" },
        }}
      >
        <LabIcon style={{ fontSize: 22 }} />
      </IconButton>
    </Tooltip>
    {/* Kept in the layout, so the row does not move when a download starts. */}
    <LinearProgress
      data-testid="project-punch-progress"
      variant={job && job.total > 0 ? "determinate" : "indeterminate"}
      value={jobPercent(job)}
      color="secondary"
      sx={{
        width: 26,
        height: 2,
        mt: "4px",
        borderRadius: 1,
        visibility: job ? "visible" : "hidden",
      }}
    />
  </div>
);

export const ProjectPunchButton = React.memo(function ProjectPunchButton() {
  const job = useProjectPunch((s) => s.state?.job ?? null);
  useEffect(() => watchProjectPunch(), []);
  return <ProjectPunchButtonView job={job} onClick={openProjectPunchDialog} />;
});
