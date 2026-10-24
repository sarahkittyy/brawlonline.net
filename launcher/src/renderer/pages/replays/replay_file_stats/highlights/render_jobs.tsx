/**
 * The stats page's renders (docs/design/highlights.md §5): this replay's render jobs, newest
 * first, with their progress (rendering has no Dolphin window: this is all the player sees of it),
 * and once done where the video is. A finished one goes: "Saved" by itself DONE_SHOWN_MS after it
 * was first shown (the mouse over it holds it), an error once its X is clicked.
 */
import type { RenderJobInfo } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import CheckCircleIcon from "@mui/icons-material/CheckCircle";
import CloseIcon from "@mui/icons-material/Close";
import ErrorOutlineIcon from "@mui/icons-material/ErrorOutline";
import MovieIcon from "@mui/icons-material/Movie";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Tooltip from "@mui/material/Tooltip";
import { useEffect, useState } from "react";

import { useServices } from "@/services";

import { HighlightListMessages as Messages } from "./highlight_list.messages";
import styles from "./highlight_list.module.css";

/** Every render job of this launcher run, kept current from RENDER_PROGRESS events. */
export function useRenderJobs(): RenderJobInfo[] {
  const { dolphinService } = useServices();
  const [jobs, setJobs] = useState<Map<string, RenderJobInfo>>(new Map());
  useEffect(() => {
    let live = true;
    const off = dolphinService.onEvent(DolphinEventType.RENDER_PROGRESS, ({ job }) => {
      setJobs((m) => new Map(m).set(job.jobId, job));
    });
    void dolphinService.getRenderJobs().then((list) => {
      if (live) {
        // (Events that came meanwhile are newer.)
        setJobs((m) => new Map([...list.map((j) => [j.jobId, j] as const), ...m]));
      }
    });
    return () => {
      live = false;
      off();
    };
  }, [dolphinService]);
  return [...jobs.values()];
}

/** How long a "Saved" row stays after it is first shown. */
const DONE_SHOWN_MS = 10_000;

// Kept for the launcher run, across pages (main keeps every job; coming back to a replay doesn't
// bring its old renders back): the rows closed with their X, and when each "Saved" was first shown.
const closed = new Set<string>();
const doneSince = new Map<string, number>();

export const RenderJobs = ({ jobs }: { jobs: RenderJobInfo[] }) => {
  const { dolphinService } = useServices();
  const [hovered, setHovered] = useState<string | null>(null);
  const [, setTick] = useState(0);
  const now = Date.now();
  for (const job of jobs) {
    if (job.state === "done" && !doneSince.has(job.jobId)) {
      doneSince.set(job.jobId, now);
    }
  }
  const shown = jobs.filter(
    (j) =>
      !closed.has(j.jobId) &&
      !(j.state === "done" && j.jobId !== hovered && now - (doneSince.get(j.jobId) ?? now) >= DONE_SHOWN_MS),
  );
  // The next "Saved" to go: draw again when it does.
  const nextGone = Math.min(
    ...shown
      .filter((j) => j.state === "done" && j.jobId !== hovered)
      .map((j) => doneSince.get(j.jobId)! + DONE_SHOWN_MS),
  );
  useEffect(() => {
    if (!Number.isFinite(nextGone)) {
      return;
    }
    const timer = setTimeout(() => setTick((t) => t + 1), Math.max(0, nextGone - Date.now()));
    return () => clearTimeout(timer);
  }, [nextGone]);

  if (!shown.length) {
    return null;
  }
  return (
    <div className={styles.renders}>
      {[...shown].reverse().map((job) => (
        <RenderJobRow
          key={job.jobId}
          job={job}
          cancel={() => void dolphinService.cancelRender(job.jobId)}
          close={() => {
            closed.add(job.jobId);
            setTick((t) => t + 1);
          }}
          hover={(over) => {
            if (over) {
              setHovered(job.jobId);
            } else {
              // (A "Saved" left gets its time again from here.)
              if (doneSince.has(job.jobId)) {
                doneSince.set(job.jobId, Date.now());
              }
              setHovered((h) => (h === job.jobId ? null : h));
            }
          }}
        />
      ))}
    </div>
  );
};

function statusText(job: RenderJobInfo): string {
  switch (job.state) {
    case "queued":
      return Messages.renderQueued();
    case "starting":
      return Messages.renderStarting();
    case "rendering":
      return job.clips > 1
        ? Messages.renderingOf(Math.max(job.clip, 1), job.clips, Math.round(job.progress * 100))
        : Messages.rendering(Math.round(job.progress * 100));
    case "done":
      return Messages.renderDone();
    case "cancelled":
      return Messages.renderCancelled();
    default:
      return Messages.renderError(job.error ?? "");
  }
}

/** "C:\x\name.mp4" (or "/x/name.mp4") -> "name" */
function fileBaseName(file: string): string {
  return (file.split(/[\\/]/).pop() ?? file).replace(/\.mp4$/, "");
}

const RenderJobRow = ({
  job,
  cancel,
  close,
  hover,
}: {
  job: RenderJobInfo;
  cancel: () => void;
  close: () => void;
  hover: (over: boolean) => void;
}) => {
  const active = job.state === "queued" || job.state === "starting" || job.state === "rendering";
  const output = job.output;
  // (The file's own name once it is chosen: " (2)" when the name was taken.)
  const name = output ? fileBaseName(output) : job.name;
  return (
    <div
      className={`${styles.renderJob} ${job.state === "error" ? styles.error : ""}`}
      onMouseEnter={() => hover(true)}
      onMouseLeave={() => hover(false)}
    >
      <div className={styles.renderLine}>
        {job.state === "done" ? (
          <CheckCircleIcon fontSize="small" className={styles.renderDoneIcon} />
        ) : job.state === "error" ? (
          <ErrorOutlineIcon fontSize="small" />
        ) : (
          <MovieIcon fontSize="small" className={styles.renderIcon} />
        )}
        <span className={styles.renderName} title={output ?? name}>
          {name}
        </span>
        <span className={styles.renderMusic}>
          {Messages.resolution(job.resolution)} · {job.music ? Messages.withMusic() : Messages.noMusic()}
        </span>
        <span className={styles.renderStatus}>{statusText(job)}</span>
        {active && (
          <Button size="small" onClick={cancel}>
            {Messages.cancel()}
          </Button>
        )}
        {job.state === "done" && output && (
          <Button size="small" onClick={() => window.electron.shell.showItemInFolder(output)}>
            {Messages.showInFolder()}
          </Button>
        )}
        {!active && (
          <Tooltip title={Messages.close()}>
            <IconButton size="small" onClick={close} aria-label={Messages.close()}>
              <CloseIcon sx={{ fontSize: 18 }} />
            </IconButton>
          </Tooltip>
        )}
      </div>
      {active && (
        <LinearProgress
          className={styles.progress}
          variant={job.state === "rendering" ? "determinate" : "indeterminate"}
          value={job.progress * 100}
        />
      )}
    </div>
  );
};
