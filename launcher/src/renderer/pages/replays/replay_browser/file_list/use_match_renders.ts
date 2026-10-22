import type { RenderJobInfo } from "@dolphin/types";
import { useEffect, useMemo, useRef } from "react";

import { useToasts } from "@/lib/hooks/use_toasts";

import { useRenderJobs } from "../../replay_file_stats/highlights/render_jobs";
import { ReplayFileMessages as Messages } from "../replay_file/replay_file.messages";

const ACTIVE: RenderJobInfo["state"][] = ["queued", "starting", "rendering"];

/** "C:\x\name.mp4" (or "/x/name.mp4") -> "name.mp4" */
const fileName = (file: string) => file.split(/[\\/]/).pop() ?? file;

/**
 * The replays list's whole-match renders (Render to video): the one still going of each replay, by
 * its path, for its row's button. One that ends while the list is open says so (its video is also
 * on the replay's page, with Show in folder).
 */
export function useMatchRenders(): Map<string, RenderJobInfo> {
  const jobs = useRenderJobs();
  const { showSuccess, showError } = useToasts();
  const seen = useRef(new Map<string, RenderJobInfo["state"]>());
  useEffect(() => {
    for (const job of jobs) {
      if (!job.wholeMatch) {
        continue;
      }
      const before = seen.current.get(job.jobId);
      seen.current.set(job.jobId, job.state);
      if (!before || !ACTIVE.includes(before)) {
        continue;
      }
      if (job.state === "done") {
        showSuccess(Messages.matchRendered(job.output ? fileName(job.output) : job.name));
      } else if (job.state === "error") {
        showError(Messages.matchRenderFailed(job.error ?? ""));
      }
    }
  }, [jobs, showSuccess, showError]);
  return useMemo(
    () => new Map(jobs.filter((j) => j.wholeMatch && ACTIVE.includes(j.state)).map((j) => [j.paths[0], j])),
    [jobs],
  );
}
