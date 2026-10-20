import { Worker } from "threads";
import type { RegisteredWorker } from "utils/register_worker";
import { registerWorker } from "utils/register_worker";

import type { WorkerSpec } from "./highlights.worker";

export type HighlightsWorker = RegisteredWorker<WorkerSpec>;

export async function createHighlightsWorker(): Promise<HighlightsWorker> {
  return registerWorker<WorkerSpec>("highlights", new Worker("./highlights.worker"));
}
