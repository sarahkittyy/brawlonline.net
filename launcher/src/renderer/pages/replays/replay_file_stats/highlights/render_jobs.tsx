/**
 * The stats page's renders (docs/design/highlights.md §5): this replay's render jobs, newest
 * first, with their progress (rendering has no Dolphin window: this is all the player sees of it),
 * and once done where the video is.
 */
import type { RenderJobInfo } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import CheckCircleIcon from "@mui/icons-material/CheckCircle";
import ErrorOutlineIcon from "@mui/icons-material/ErrorOutline";
import MovieIcon from "@mui/icons-material/Movie";
import Button from "@mui/material/Button";
import LinearProgress from "@mui/material/LinearProgress";
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

export const RenderJobs = ({ jobs }: { jobs: RenderJobInfo[] }) => {
  const { dolphinService } = useServices();
  if (!jobs.length) {
    return null;
  }
  return (
    <div className={styles.renders}>
      {[...jobs].reverse().map((job) => (
        <RenderJobRow key={job.jobId} job={job} cancel={() => void dolphinService.cancelRender(job.jobId)} />
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

const RenderJobRow = ({ job, cancel }: { job: RenderJobInfo; cancel: () => void }) => {
  const active = job.state === "queued" || job.state === "starting" || job.state === "rendering";
  const output = job.output;
  // (The file's own name once it is chosen: " (2)" when the name was taken.)
  const name = output ? fileBaseName(output) : job.name;
  return (
    <div className={`${styles.renderJob} ${job.state === "error" ? styles.error : ""}`}>
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
