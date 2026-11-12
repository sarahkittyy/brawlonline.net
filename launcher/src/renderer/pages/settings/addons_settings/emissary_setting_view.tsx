// Settings > Add-ons' EmissaryRecomp card (addons_settings.tsx): what is installed and the latest,
// the program's and the game files' sizes, Download / Update / Prepare game files / Remove, and
// the auto-update switch (docs/design/brawl-recompiled.md §4.1).
import { formatBytes } from "@common/disk_space";
import type { EmissaryState } from "@dolphin/emissary/types";
import { EMISSARY_AUTHOR } from "@dolphin/emissary/types";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import DownloadRoundedIcon from "@mui/icons-material/DownloadRounded";
import FolderOpenRoundedIcon from "@mui/icons-material/FolderOpenRounded";
import GitHubIcon from "@mui/icons-material/GitHub";
import Button from "@mui/material/Button";
import LinearProgress from "@mui/material/LinearProgress";
import Link from "@mui/material/Link";
import React from "react";

import { Toggle } from "@/components/form/toggle";
import { emissaryAction, emissaryJobPercent } from "@/lib/emissary/use_emissary";

import { SettingItem } from "../setting_item_section";
import { AddonsSettingsMessages as Messages } from "./addons_settings.messages";

export type EmissarySettingViewProps = {
  state: EmissaryState | null;
  autoUpdate: boolean;
  busy?: boolean;
  onGet: () => void;
  onCancel: () => void;
  onRemove: () => void;
  onOpenFolder: () => void;
  onOpenRepo: () => void;
  onChooseDisc: () => void;
  onAutoUpdate: (value: boolean) => void;
};

const Row = ({ label, children }: { label: string; children: React.ReactNode }) => (
  <div style={{ display: "flex", alignItems: "center", minHeight: 30, gap: 12 }}>
    <span style={{ width: 90, flexShrink: 0, opacity: 0.6, fontSize: 14 }}>{label}</span>
    <div style={{ display: "flex", alignItems: "center", gap: 6, minWidth: 0, fontSize: 14 }}>{children}</div>
  </div>
);

const jobText = (job: NonNullable<EmissaryState["job"]>) =>
  job.phase === "download"
    ? Messages.jobDownload()
    : job.phase === "unpack"
    ? Messages.jobUnpack()
    : Messages.jobGameFiles();

export const EmissarySettingView = ({
  state,
  autoUpdate,
  busy,
  onGet,
  onCancel,
  onRemove,
  onOpenFolder,
  onOpenRepo,
  onChooseDisc,
  onAutoUpdate,
}: EmissarySettingViewProps) => {
  const installed = state?.installed ?? null;
  const job = state?.job ?? null;
  const action = emissaryAction(state);
  const running = state?.running ?? false;
  return (
    <SettingItem
      name={
        <span style={{ display: "inline-flex", alignItems: "center", gap: 8 }}>
          {Messages.emissary()}
          <span
            style={{
              fontSize: 11,
              fontWeight: 700,
              letterSpacing: 0.5,
              padding: "1px 6px",
              borderRadius: 4,
              border: "1px solid currentColor",
              opacity: 0.8,
            }}
          >
            {Messages.beta()}
          </span>
        </span>
      }
    >
      <div
        data-testid="emissary-card"
        style={{ padding: "12px 16px", borderRadius: 8, background: "rgba(0, 0, 0, 0.25)" }}
      >
        <div style={{ fontSize: 13, opacity: 0.75, marginBottom: 8 }}>
          {Messages.emissaryDescription()} {Messages.by()}{" "}
          <Link
            component="button"
            onClick={onOpenRepo}
            underline="hover"
            style={{ fontSize: "inherit", fontWeight: 600 }}
          >
            <GitHubIcon style={{ fontSize: "1em", verticalAlign: "-0.12em", marginRight: 4 }} />
            {EMISSARY_AUTHOR}
          </Link>
        </div>
        <Row label={Messages.status()}>
          <span data-testid="emissary-status">{state ? Messages.statusText(state.status) : Messages.unknown()}</span>
        </Row>
        <Row label={Messages.installed()}>
          <span style={{ fontWeight: installed ? 600 : undefined }}>
            {installed ? installed.version : Messages.notInstalled()}
          </span>
        </Row>
        <Row label={Messages.latest()}>
          <span>{state?.latest?.version ?? Messages.unknown()}</span>
        </Row>
        {installed && (
          <Row label={Messages.program()}>
            <span>{formatBytes(installed.programBytes)}</span>
          </Row>
        )}
        {state?.gameFiles && (
          <Row label={Messages.gameFiles()}>
            <span>
              {formatBytes(state.gameFiles.bytes)} {Messages.fromDisc()}
            </span>
          </Row>
        )}
        {state?.staged && (
          <div style={{ marginTop: 8, fontSize: 13, opacity: 0.85 }}>{Messages.staged(state.staged)}</div>
        )}

        {state?.error && !job && <div style={{ marginTop: 8, color: "#f48a8a", fontSize: 13 }}>{state.error}</div>}

        {job ? (
          <div style={{ marginTop: 12 }}>
            <div style={{ fontSize: 13, opacity: 0.75, marginBottom: 4 }}>
              {jobText(job)}
              {job.phase === "download" && job.total > 0
                ? ` ${formatBytes(job.current)} / ${formatBytes(job.total)}`
                : job.phase === "gameFiles"
                ? ` ${Math.round(emissaryJobPercent(job))}%`
                : ""}
            </div>
            <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
              <LinearProgress
                style={{ flex: 1, borderRadius: 2 }}
                variant={job.total > 0 ? "determinate" : "indeterminate"}
                value={emissaryJobPercent(job)}
              />
              <Button size="small" variant="outlined" color="secondary" onClick={onCancel}>
                {Messages.cancel()}
              </Button>
            </div>
          </div>
        ) : (
          <div style={{ display: "flex", gap: 8, marginTop: 12, flexWrap: "wrap", alignItems: "center" }}>
            {state?.status === "needsDisc" && (
              <Button size="small" variant="contained" color="secondary" onClick={onChooseDisc}>
                {Messages.chooseDisc()}
              </Button>
            )}
            {action && (
              <Button
                size="small"
                variant="contained"
                color="secondary"
                startIcon={<DownloadRoundedIcon />}
                disabled={busy || running}
                onClick={onGet}
              >
                {action === "download"
                  ? Messages.download(formatBytes(state!.downloadBytes), formatBytes(state!.diskBytes))
                  : action === "update"
                  ? Messages.update(formatBytes(state!.downloadBytes))
                  : Messages.prepare(formatBytes(state!.diskBytes))}
              </Button>
            )}
            {installed && (
              <>
                <Button
                  size="small"
                  variant="outlined"
                  color="secondary"
                  startIcon={<FolderOpenRoundedIcon />}
                  onClick={onOpenFolder}
                >
                  {Messages.openFolder()}
                </Button>
                <Button
                  size="small"
                  variant="outlined"
                  color="secondary"
                  startIcon={<DeleteOutlineRoundedIcon />}
                  disabled={busy || running}
                  onClick={onRemove}
                  style={{ marginLeft: "auto" }}
                >
                  {Messages.remove()}
                </Button>
              </>
            )}
          </div>
        )}
        {state?.supported !== false && (
          <div style={{ marginTop: 12 }}>
            <Toggle
              value={autoUpdate}
              onChange={onAutoUpdate}
              label={Messages.autoUpdate()}
              description={Messages.autoUpdateDescription()}
            />
          </div>
        )}
        {installed && state?.status === "ready" && (
          <div style={{ marginTop: 8, fontSize: 13, opacity: 0.75 }}>{Messages.playWith()}</div>
        )}
      </div>
    </SettingItem>
  );
};
