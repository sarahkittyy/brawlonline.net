// EmissaryRecomp's popup: Play with EmissaryRecomp chosen (Settings > Game, or the Add-ons card's switch) and it is
// not installed yet. Project Punch's popup (project_punch_dialog_view.tsx), the same layout and words: what it is,
// then Download, its progress with Cancel, then Play. A download keeps going when it is closed (the sidebar shows it).
import { formatBytes } from "@common/disk_space";
import type { EmissaryState } from "@dolphin/emissary/types";
import { EMISSARY_AUTHOR } from "@dolphin/emissary/types";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import DownloadRoundedIcon from "@mui/icons-material/DownloadRounded";
import GitHubIcon from "@mui/icons-material/GitHub";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Link from "@mui/material/Link";

import { ProjectPunchMessages } from "@/lib/project_punch/project_punch.messages";
import { AddonsSettingsMessages as Messages } from "@/pages/settings/addons_settings/addons_settings.messages";

import { emissaryAction, emissaryJobPercent } from "./use_emissary";

export type EmissaryDialogViewProps = {
  open: boolean;
  state: EmissaryState | null;
  /** Play was pressed and the game is starting. */
  starting?: boolean;
  onClose: () => void;
  onGet: () => void;
  onCancel: () => void;
  onPlay: () => void;
  onOpenRepo: () => void;
};

/** It can play now: installed with online play, its files there, nothing running in its place. */
const canPlay = (state: EmissaryState | null) =>
  state?.installed?.playsOnline === true &&
  state.job == null &&
  (state.status === "ready" || state.status === "updateAvailable");

export const EmissaryDialogView = ({
  open,
  state,
  starting,
  onClose,
  onGet,
  onCancel,
  onPlay,
  onOpenRepo,
}: EmissaryDialogViewProps) => {
  const job = state?.job ?? null;
  const installed = state?.installed ?? null;
  const action = job ? null : emissaryAction(state);
  const status = job
    ? null
    : !installed
    ? ProjectPunchMessages.notDownloaded()
    : state && (state.status === "needsFiles" || state.status === "needsDisc")
    ? Messages.statusText(state.status)
    : null;

  return (
    <Dialog open={open} onClose={onClose} fullWidth={true} PaperProps={{ style: { maxWidth: 520 } }}>
      <DialogTitle style={{ fontSize: 22, fontWeight: 700, paddingRight: 48, whiteSpace: "nowrap" }}>
        <span style={{ display: "inline-flex", alignItems: "center", gap: 10 }}>
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
        <IconButton
          aria-label={ProjectPunchMessages.close()}
          onClick={onClose}
          size="small"
          style={{ position: "absolute", right: 12, top: 14 }}
        >
          <CloseRoundedIcon fontSize="small" />
        </IconButton>
      </DialogTitle>
      <DialogContent>
        <p style={{ marginTop: 0, lineHeight: 1.5 }}>
          {Messages.emissaryDescription()} {Messages.madeBy()}{" "}
          <Link
            component="button"
            onClick={onOpenRepo}
            underline="hover"
            data-testid="emissary-repo"
            style={{ verticalAlign: "baseline", fontSize: "inherit", fontWeight: 600 }}
          >
            <GitHubIcon style={{ fontSize: "1em", verticalAlign: "-0.12em", marginRight: 4 }} />
            {EMISSARY_AUTHOR}
          </Link>
        </p>

        {status && <p style={{ margin: "16px 0 0", opacity: 0.75 }}>{status}</p>}
        {state?.error && !job && <p style={{ margin: "8px 0 0", color: "#f48a8a", fontSize: 13 }}>{state.error}</p>}

        {job && (
          <div style={{ marginTop: 16 }}>
            <div style={{ display: "flex", fontSize: 13, opacity: 0.8 }}>
              <span>
                {job.phase === "download"
                  ? Messages.jobDownload()
                  : job.phase === "unpack"
                  ? Messages.jobUnpack()
                  : Messages.jobGameFiles()}
              </span>
              {job.phase === "download" && job.total > 0 && (
                <span style={{ marginLeft: "auto", fontVariantNumeric: "tabular-nums" }}>
                  {formatBytes(job.current)} / {formatBytes(job.total)}
                </span>
              )}
              {job.phase === "gameFiles" && (
                <span style={{ marginLeft: "auto", fontVariantNumeric: "tabular-nums" }}>
                  {Math.round(emissaryJobPercent(job))}%
                </span>
              )}
            </div>
            <LinearProgress
              style={{ marginTop: 6, borderRadius: 2 }}
              variant={job.total > 0 ? "determinate" : "indeterminate"}
              value={emissaryJobPercent(job)}
            />
          </div>
        )}

        <div style={{ display: "flex", gap: 8, justifyContent: "flex-end", marginTop: 20, flexWrap: "wrap" }}>
          {job ? (
            <Button variant="outlined" color="secondary" onClick={onCancel} data-testid="emissary-cancel">
              {Messages.cancel()}
            </Button>
          ) : (
            <>
              {state?.status === "needsDisc" ? null : action && action !== "update" ? (
                <Button
                  variant="contained"
                  color="secondary"
                  startIcon={<DownloadRoundedIcon />}
                  onClick={onGet}
                  data-testid="emissary-get"
                >
                  {action === "download"
                    ? Messages.download(formatBytes(state!.downloadBytes), formatBytes(state!.diskBytes))
                    : Messages.prepare(formatBytes(state!.diskBytes))}
                </Button>
              ) : null}
              {installed && canPlay(state) && (
                <Button
                  variant="contained"
                  color="secondary"
                  startIcon={starting ? <CircularProgress color="inherit" size={16} /> : <PlayArrowRoundedIcon />}
                  disabled={starting}
                  onClick={onPlay}
                  data-testid="emissary-play"
                >
                  {ProjectPunchMessages.play(installed.version)}
                </Button>
              )}
            </>
          )}
        </div>
      </DialogContent>
    </Dialog>
  );
};
