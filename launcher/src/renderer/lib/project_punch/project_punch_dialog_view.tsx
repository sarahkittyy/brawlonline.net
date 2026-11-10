// Project Punch's popup (Home's lab button): what it is, then Download / Update / Play. A download
// keeps going when it is closed (the lab button shows a thin bar under it), and Cancel stops it.
import { formatBytes } from "@common/disk_space";
import type { ProjectPunchState } from "@dolphin/project_punch/types";
import { PROJECT_PUNCH_AUTHOR } from "@dolphin/project_punch/types";
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

import { ProjectPunchMessages as Messages } from "./project_punch.messages";
import { jobPercent, projectPunchActions } from "./project_punch_actions";

export type ProjectPunchDialogViewProps = {
  open: boolean;
  state: ProjectPunchState | null;
  /** Play was pressed and the game is starting. */
  starting?: boolean;
  onClose: () => void;
  onGet: () => void;
  onCancel: () => void;
  onPlay: () => void;
  onRetry: () => void;
  onOpenRepo: () => void;
};

export const ProjectPunchDialogView = ({
  open,
  state,
  starting,
  onClose,
  onGet,
  onCancel,
  onPlay,
  onRetry,
  onOpenRepo,
}: ProjectPunchDialogViewProps) => {
  const { installed, job, canPlay, get, offline } = projectPunchActions(state);
  const status = job
    ? null
    : offline
    ? Messages.offline()
    : !installed
    ? Messages.notDownloaded()
    : get === "update"
    ? Messages.notLatest()
    : null;
  const size = formatBytes(state?.downloadBytes ?? 0);

  return (
    <Dialog open={open} onClose={onClose} fullWidth={true} PaperProps={{ style: { maxWidth: 520 } }}>
      <DialogTitle style={{ fontSize: 22, fontWeight: 700, paddingRight: 48, whiteSpace: "nowrap" }}>
        {Messages.title()}
        <IconButton
          aria-label={Messages.close()}
          onClick={onClose}
          size="small"
          style={{ position: "absolute", right: 12, top: 14 }}
        >
          <CloseRoundedIcon fontSize="small" />
        </IconButton>
      </DialogTitle>
      <DialogContent>
        <p style={{ marginTop: 0, lineHeight: 1.5 }}>
          {Messages.descriptionBefore()}{" "}
          <Link
            component="button"
            onClick={onOpenRepo}
            underline="hover"
            data-testid="project-punch-repo"
            style={{ verticalAlign: "baseline", fontSize: "inherit", fontWeight: 600 }}
          >
            <GitHubIcon style={{ fontSize: "1em", verticalAlign: "-0.12em", marginRight: 4 }} />
            {PROJECT_PUNCH_AUTHOR}
          </Link>{" "}
          {Messages.descriptionAfter()}
          <br />
          {Messages.offlineOnly()}
        </p>

        {status && <p style={{ margin: "16px 0 0", opacity: 0.75 }}>{status}</p>}
        {state?.error && !job && <p style={{ margin: "8px 0 0", color: "#f48a8a", fontSize: 13 }}>{state.error}</p>}

        {job && (
          <div style={{ marginTop: 16 }}>
            <div style={{ display: "flex", fontSize: 13, opacity: 0.8 }}>
              <span>{job.phase === "download" ? Messages.downloading() : Messages.makingCard()}</span>
              {job.total > 1 && (
                <span style={{ marginLeft: "auto", fontVariantNumeric: "tabular-nums" }}>
                  {formatBytes(job.current)} / {formatBytes(job.total)}
                </span>
              )}
            </div>
            <LinearProgress
              style={{ marginTop: 6, borderRadius: 2 }}
              variant={job.total > 0 ? "determinate" : "indeterminate"}
              value={jobPercent(job)}
            />
          </div>
        )}

        <div style={{ display: "flex", gap: 8, justifyContent: "flex-end", marginTop: 20, flexWrap: "wrap" }}>
          {job ? (
            <Button variant="outlined" color="secondary" onClick={onCancel} data-testid="project-punch-cancel">
              {Messages.cancel()}
            </Button>
          ) : (
            <>
              {offline && (
                <Button variant="outlined" color="secondary" onClick={onRetry}>
                  {Messages.retry()}
                </Button>
              )}
              {get && (
                <Button
                  variant={canPlay ? "outlined" : "contained"}
                  color="secondary"
                  startIcon={<DownloadRoundedIcon />}
                  onClick={onGet}
                  data-testid="project-punch-get"
                >
                  {get === "update" ? Messages.update(size) : Messages.download(size)}
                </Button>
              )}
              {installed && canPlay && (
                <Button
                  variant="contained"
                  color="secondary"
                  startIcon={starting ? <CircularProgress color="inherit" size={16} /> : <PlayArrowRoundedIcon />}
                  disabled={starting || state?.running}
                  onClick={onPlay}
                  data-testid="project-punch-play"
                >
                  {Messages.play(installed.version)}
                </Button>
              )}
            </>
          )}
        </div>
      </DialogContent>
    </Dialog>
  );
};
