// Plays replays on the Project+ release they were recorded on (docs/design/pplus-versions.md):
// main says what a request needs (ReplayPlayResult); this asks the player, shows the download and
// the card being made, and plays. Rendering goes through `withRenderVersion` the same way (with no
// "anyway": a clip is rendered on its own release or not at all).
import { pplusReleaseLabel } from "@common/pplus_releases";
import type { RenderJobInfo, RenderResult, ReplayPlayChoice, ReplayPlayResult } from "@dolphin/types";

import type { AskAnswer, VersionPurpose } from "./replay_version_dialog";
import {
  askForRelease,
  closeReleaseDialog,
  showReleaseError,
  showUnplayable,
  showWorking,
} from "./replay_version_dialog";
import { ReplayVersionDialogMessages as Messages } from "./replay_version_dialog.messages";
import { refreshPPlusVersions, usePPlusVersions } from "./use_pplus_versions";

type NeedsFiles = Extract<ReplayPlayResult, { status: "needs-files" }>;

/**
 * Gets `need.release` ready (asks first). Resolves to `ready`, `current`
 * (the player chose to play on the launcher's release), or `cancel`.
 */
export async function ensureReplayRelease(
  need: Omit<NeedsFiles, "status">,
  purpose: VersionPurpose = "play",
): Promise<"ready" | "current" | "cancel"> {
  const { release, current } = need;
  for (;;) {
    const ask: AskAnswer = await askForRelease({ ...need, purpose });
    if (ask.action === "cancel") {
      closeReleaseDialog();
      return "cancel";
    }
    if (ask.action === "current") {
      closeReleaseDialog();
      return "current";
    }
    const importFrom = ask.action === "import" ? ask.path : undefined;
    for (;;) {
      showWorking({ release, current, purpose, importing: importFrom != null });
      const result = await window.electron.dolphin.preparePPlusRelease(release, importFrom);
      void refreshPPlusVersions();
      if (result.ok) {
        closeReleaseDialog();
        return "ready";
      }
      const next = await showReleaseError({ release, current, purpose, code: result.code, message: result.message });
      if (next !== "retry") {
        closeReleaseDialog();
        return next === "current" && purpose === "play" ? "current" : "cancel";
      }
      // A folder that was not that release: back to the choice (download, or another folder).
      if (importFrom) {
        break;
      }
    }
  }
}

/**
 * Runs a play request (`call`, with the choice main should make) through the version checks.
 * Resolves to what was played, or null when the player cancelled.
 */
export async function withReplayVersion(
  call: (choice: ReplayPlayChoice) => Promise<ReplayPlayResult>,
  notify?: (message: string) => void,
): Promise<ReplayPlayResult | null> {
  let result = await call("auto");
  for (let tries = 0; tries < 3; tries++) {
    if (result.status === "playing") {
      if (result.skipped > 0 && notify) {
        notify(Messages.skipped(pplusReleaseLabel(result.release ?? usePPlusVersions.getState().current ?? "")));
      }
      return result;
    }
    if (result.status === "unplayable") {
      const next = await showUnplayable({ release: result.release, current: result.current, purpose: "play" });
      closeReleaseDialog();
      if (next !== "current") {
        return null;
      }
      result = await call("current");
      continue;
    }
    const ready = await ensureReplayRelease(result, "play");
    if (ready === "cancel") {
      return null;
    }
    result = await call(ready === "current" ? "current" : "auto");
  }
  return result;
}

/**
 * Runs a render request (`call`) through the version checks: the releases its replays need are got
 * ready first (asked). Resolves to the jobs queued, or null when the player cancelled or a replay's
 * release can't be rendered.
 */
export async function withRenderVersion(
  call: (choice: ReplayPlayChoice) => Promise<RenderResult>,
): Promise<RenderJobInfo[] | null> {
  let result = await call("auto");
  // (One release at a time: a combo of replays from two older releases asks twice.)
  for (let tries = 0; tries < 4; tries++) {
    if (result.status === "queued") {
      return result.jobs;
    }
    if (result.status === "unplayable") {
      await showUnplayable({ release: result.release, current: result.current, purpose: "render" });
      closeReleaseDialog();
      return null;
    }
    const ready = await ensureReplayRelease(result, "render");
    if (ready !== "ready") {
      return null;
    }
    result = await call("auto");
  }
  return null;
}
