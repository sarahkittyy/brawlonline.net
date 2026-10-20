// The highlight detector off the launcher's main thread (DESIGN.md §1: a worker_threads worker).
// NOTE: like the replay indexing worker, this module cannot use electron-log.

import type { ModuleMethods } from "threads/dist/types/master";
import { expose } from "threads/worker";

import type { AnalyzeOptions } from "./analyze_replay";
import { analyzeReplay } from "./analyze_replay";
import type { ReplayHighlights } from "./types";

interface Methods {
  dispose: () => Promise<void>;
  analyzeReplay(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights>;
}

export type WorkerSpec = ModuleMethods & Methods;

const methods: WorkerSpec = {
  async dispose(): Promise<void> {
    // Nothing to clean up.
  },
  async analyzeReplay(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
    return analyzeReplay(fullPath, options);
  },
};

expose(methods);
