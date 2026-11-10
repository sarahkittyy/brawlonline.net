// Settings > Training's Project Punch block (training_settings.tsx): where its SD card is, which
// version is on it and which is the latest; Download / Update / Play / Remove.
import { formatBytes } from "@common/disk_space";
import type { ProjectPunchState } from "@dolphin/project_punch/types";
import { PROJECT_PUNCH_AUTHOR } from "@dolphin/project_punch/types";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import DownloadRoundedIcon from "@mui/icons-material/DownloadRounded";
import FolderOpenRoundedIcon from "@mui/icons-material/FolderOpenRounded";
import GitHubIcon from "@mui/icons-material/GitHub";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Link from "@mui/material/Link";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { LabIcon } from "@/lib/project_punch/lab_icon";
import { ProjectPunchMessages } from "@/lib/project_punch/project_punch.messages";
import { jobPercent, projectPunchActions } from "@/lib/project_punch/project_punch_actions";

import { SettingItem } from "../setting_item_section";
import { TrainingSettingsMessages as Messages } from "./training_settings.messages";

export type ProjectPunchSettingViewProps = {
  state: ProjectPunchState | null;
  busy?: boolean;
  onGet: () => void;
  onCancel: () => void;
  onPlay: () => void;
  onRemove: () => void;
  onShowCard: () => void;
  onOpenRepo: () => void;
};

const Row = ({ label, children }: { label: string; children: React.ReactNode }) => (
  <div style={{ display: "flex", alignItems: "center", minHeight: 30, gap: 12 }}>
    <span style={{ width: 90, flexShrink: 0, opacity: 0.6, fontSize: 14 }}>{label}</span>
    <div style={{ display: "flex", alignItems: "center", gap: 6, minWidth: 0, fontSize: 14 }}>{children}</div>
  </div>
);

export const ProjectPunchSettingView = ({
  state,
  busy,
  onGet,
  onCancel,
  onPlay,
  onRemove,
  onShowCard,
  onOpenRepo,
}: ProjectPunchSettingViewProps) => {
  const { installed, job, canPlay, get } = projectPunchActions(state);
  const size = formatBytes(state?.downloadBytes ?? 0);
  return (
    <SettingItem
      name={
        <span style={{ display: "inline-flex", alignItems: "center", gap: 8 }}>
          <LabIcon style={{ fontSize: 20 }} />
          {Messages.projectPunch()}
        </span>
      }
    >
      <div style={{ padding: "12px 16px", borderRadius: 8, background: "rgba(0, 0, 0, 0.25)" }}>
        <div style={{ fontSize: 13, opacity: 0.75, marginBottom: 8 }}>
          {Messages.by()}{" "}
          <Link
            component="button"
            onClick={onOpenRepo}
            underline="hover"
            style={{ fontSize: "inherit", fontWeight: 600 }}
          >
            <GitHubIcon style={{ fontSize: "1em", verticalAlign: "-0.12em", marginRight: 4 }} />
            {PROJECT_PUNCH_AUTHOR}
          </Link>
        </div>
        <Row label={Messages.installed()}>
          <span data-testid="project-punch-installed" style={{ fontWeight: installed ? 600 : undefined }}>
            {installed ? installed.version : Messages.notInstalled()}
          </span>
        </Row>
        <Row label={Messages.latest()}>
          <span>{state?.latest?.version ?? Messages.unknown()}</span>
        </Row>
        {installed && (
          <>
            <Row label={Messages.sdCard()}>
              <span
                style={{
                  fontFamily: "monospace",
                  fontSize: 12,
                  overflow: "hidden",
                  textOverflow: "ellipsis",
                  whiteSpace: "nowrap",
                }}
                title={state?.cardPath}
              >
                {state?.cardPath}
              </span>
              <Tooltip title={Messages.showInFolder()}>
                <IconButton size="small" onClick={onShowCard} aria-label={Messages.showInFolder()}>
                  <FolderOpenRoundedIcon fontSize="small" />
                </IconButton>
              </Tooltip>
            </Row>
            <Row label={Messages.size()}>
              <span>{formatBytes(installed.cardBytes)}</span>
            </Row>
          </>
        )}

        {state?.error && !job && <div style={{ marginTop: 8, color: "#f48a8a", fontSize: 13 }}>{state.error}</div>}

        {job ? (
          <div style={{ display: "flex", alignItems: "center", gap: 12, marginTop: 12 }}>
            <LinearProgress
              style={{ flex: 1, borderRadius: 2 }}
              variant={job.total > 0 ? "determinate" : "indeterminate"}
              value={jobPercent(job)}
            />
            <Button size="small" variant="outlined" color="secondary" onClick={onCancel}>
              {ProjectPunchMessages.cancel()}
            </Button>
          </div>
        ) : (
          <div style={{ display: "flex", gap: 8, marginTop: 12, flexWrap: "wrap" }}>
            {get && (
              <Button
                size="small"
                variant={canPlay ? "outlined" : "contained"}
                color="secondary"
                startIcon={<DownloadRoundedIcon />}
                disabled={busy || state?.running}
                onClick={onGet}
              >
                {get === "update" ? ProjectPunchMessages.update(size) : ProjectPunchMessages.download(size)}
              </Button>
            )}
            {installed && canPlay && (
              <Button
                size="small"
                variant="contained"
                color="secondary"
                startIcon={<PlayArrowRoundedIcon />}
                disabled={busy || state?.running}
                onClick={onPlay}
              >
                {ProjectPunchMessages.play(installed.version)}
              </Button>
            )}
            {installed && (
              <Button
                size="small"
                variant="outlined"
                color="secondary"
                startIcon={<DeleteOutlineRoundedIcon />}
                disabled={busy || state?.running}
                onClick={onRemove}
                style={{ marginLeft: "auto" }}
              >
                {Messages.remove()}
              </Button>
            )}
          </div>
        )}
      </div>
    </SettingItem>
  );
};
