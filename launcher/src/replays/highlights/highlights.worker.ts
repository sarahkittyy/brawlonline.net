// The highlight detector and the replay stats off the launcher's main thread (DESIGN.md §1: a
// worker_threads worker).
// NOTE: like the replay indexing worker, this module cannot use electron-log.

import type { StatsType } from "@slippi/slippi-js";
import type { ModuleMethods } from "threads/dist/types/master";
import { expose } from "threads/worker";

import { readStats } from "../stats/brawl_stats";
import type { AnalyzeOptions } from "./analyze_replay";
import { analyzeReplay } from "./analyze_replay";
import type { ReplayHighlights } from "./types";

interface Methods {
  dispose: () => Promise<void>;
  analyzeReplay(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights>;
  calculateStats(fullPath: string): Promise<StatsType>;
}

export type WorkerSpec = ModuleMethods & Methods;

const methods: WorkerSpec = {
  async dispose(): Promise<void> {
    // Nothing to clean up.
  },
  async analyzeReplay(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
    return analyzeReplay(fullPath, options);
  },
  async calculateStats(fullPath: string): Promise<StatsType> {
    return readStats(fullPath);
  },
};

expose(methods);
