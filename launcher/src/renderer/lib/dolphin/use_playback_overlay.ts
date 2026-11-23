/**
 * The playback Dolphin's seek bar, the launcher's side (`<comm>.overlay.json`, Core/Rollback/ReplayPlayback.h):
 * the launcher's colour for the bar's played part (Sarah's board, 2026-11-20: "the played part should be your
 * launcher's color"), and clip mode's Render (w/ Music) / Render (No Music): Dolphin asks (status
 * `renderRequests`), this queues the render as the Highlights tab's Render does (the clip is a Manual Clip by
 * then; a star's highlight playing has Render too, named from the replay's highlight), and the render's progress
 * shows in the game, at its top right.
 *
 * Mounted once for the whole window (app/create.tsx).
 */
import type { DolphinService, PlaybackOverlayFile, PlaybackOverlayToast, RenderJobInfo } from "@dolphin/types";
import { DolphinEventType, DolphinLaunchType } from "@dolphin/types";
import type { HighlightPlayer } from "@replays/highlights/types";
import { useEffect, useRef } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useSettingsStore } from "@/lib/hooks/use_settings";
import { HighlightListMessages as Messages } from "@/pages/replays/replay_file_stats/highlights/highlight_list.messages";
import { renderStatusText } from "@/pages/replays/replay_file_stats/highlights/render_jobs";
import { clipVideoName, replayDateOf } from "@/pages/replays/replay_file_stats/highlights/render_names";
import { useServices } from "@/services";

import type { RenderAsk } from "./playback_overlay";
import { clipLength, cssColorToHex, findClip, findHighlight } from "./playback_overlay";

/** How often the launcher's colour is looked at (the theme can change while Dolphin is open). */
const ACCENT_POLL_MS = 1000;
/** How long a render's clip may take to be kept as a Manual Clip (a spectated game is copied first). */
const CLIP_WAIT_MS = 20_000;
/** How many of this run's in-game renders the overlay file has (a star's row shows its own). */
const MAX_KEPT = 24;
/** How long a finished render stays in the game. */
const DONE_SHOWN_MS = 6000;

/** The launcher's accent colour as the page has it now (`--accent-primary`). */
function currentAccent(): string | null {
  const probe = document.createElement("span");
  probe.style.color = "var(--accent-primary)";
  probe.style.display = "none";
  document.body.appendChild(probe);
  const color = getComputedStyle(probe).color;
  probe.remove();
  return cssColorToHex(color);
}

/** A render job as the game shows it. */
export function toastOf(job: RenderJobInfo, ask: RenderAsk, label: string, toast: boolean): PlaybackOverlayToast {
  const state: PlaybackOverlayToast["state"] =
    job.state === "done"
      ? "done"
      : job.state === "error" || job.state === "cancelled"
      ? "error"
      : job.state === "rendering"
      ? "rendering"
      : "queued";
  return {
    id: job.jobId,
    path: ask.path,
    startFrame: ask.startFrame,
    endFrame: ask.endFrame,
    label,
    text: renderStatusText(job),
    progress: job.progress,
    state,
    toast,
    folder: job.state === "done" && !!job.output,
  };
}

type Tracked = { ask: RenderAsk; label: string; job: RenderJobInfo | null; doneAt: number | null };

export const usePlaybackOverlay = () => {
  const { dolphinService, replayService } = useServices();
  const { renderHighlights } = useDolphinActions(dolphinService);
  const renderRef = useRef(renderHighlights);
  renderRef.current = renderHighlights;
  const replayRef = useRef(replayService);
  replayRef.current = replayService;

  useEffect(() => {
    let accent: string | null = null;
    let lastSent = "";
    // Renders asked for in this playback Dolphin's run (its seqs start at 1 again in the next one).
    let handled = new Set<number>();
    let shownSeq = 0;
    const tracked = new Map<string, Tracked>();
    let alive = true;

    // Every render asked for from the game in this launcher run (a star's row shows its state), the
    // newest MAX_KEPT; a finished one shows at the top right for DONE_SHOWN_MS.
    const send = () => {
      const now = Date.now();
      const toasts: PlaybackOverlayToast[] = [];
      for (const t of tracked.values()) {
        if (t.job) {
          toasts.push(toastOf(t.job, t.ask, t.label, t.doneAt == null || now - t.doneAt < DONE_SHOWN_MS));
        }
      }
      const overlay: PlaybackOverlayFile = { ...(accent ? { accent } : {}), toasts: toasts.slice(-MAX_KEPT) };
      const text = JSON.stringify(overlay);
      if (text !== lastSent) {
        lastSent = text;
        void sendOverlay(dolphinService, overlay);
      }
    };

    const tick = () => {
      const a = currentAccent();
      if (a !== accent) {
        accent = a;
      }
      send();
    };
    tick();
    const timer = setInterval(tick, ACCENT_POLL_MS);

    const render = async (ask: RenderAsk) => {
      // The clip as the launcher keeps it (manual_clips.ts): its file (a spectated game's copy) and players. Clip
      // mode's is being kept now; a star's (`clip: false`) is a Manual Clip already, or one of the replay's highlights.
      const until = Date.now() + CLIP_WAIT_MS;
      let clip = findClip(useSettingsStore.getState().settings.manualClips, ask);
      while (!clip && ask.clip !== false && alive && Date.now() < until) {
        await new Promise((resolve) => setTimeout(resolve, 250));
        clip = findClip(useSettingsStore.getState().settings.manualClips, ask);
      }
      const path = clip?.path ?? ask.path;
      let name = [replayDateOf(path), "Clip"].filter(Boolean).join(" ");
      if (clip) {
        const players = new Map<number, HighlightPlayer>(clip.players.map((p) => [p.port, p]));
        name = clipVideoName(
          { kind: "manual", startFrame: ask.startFrame, endFrame: ask.endFrame, savedAt: clip.savedAt },
          players,
          path,
        );
      } else if (ask.clip === false) {
        const found = await replayRef.current.getHighlights(path).catch(() => null);
        const row = found ? findHighlight(found, ask) : null;
        if (found && row) {
          name = clipVideoName(row, new Map(found.players.map((p) => [p.port, p])), path);
        }
      }
      const label = ask.label || Messages.manualClip(clipLength(ask.startFrame, ask.endFrame));
      const jobs = await renderRef.current({
        items: [{ path, startFrame: ask.startFrame, endFrame: ask.endFrame }],
        names: [name],
        music: ask.music,
        combo: false,
      });
      for (const job of jobs ?? []) {
        tracked.set(job.jobId, { ask, label, job, doneAt: null });
      }
      send();
    };

    const offStatus = dolphinService.onEvent(DolphinEventType.PLAYBACK_STATUS, ({ instanceId, status }) => {
      if (instanceId !== "playback") {
        return;
      }
      for (const ask of status.renderRequests ?? []) {
        if (!ask || !Number.isFinite(ask.seq) || handled.has(ask.seq) || typeof ask.path !== "string") {
          continue;
        }
        handled.add(ask.seq);
        void render(ask).catch(() => undefined);
      }
      // A finished render's folder button.
      const show = status.showRender;
      if (show && Number.isFinite(show.seq) && show.seq > shownSeq) {
        shownSeq = show.seq;
        const output = tracked.get(show.id)?.job?.output;
        if (output) {
          window.electron.shell.showItemInFolder(output);
        }
      }
    });
    const offProgress = dolphinService.onEvent(DolphinEventType.RENDER_PROGRESS, ({ job }) => {
      const t = tracked.get(job.jobId);
      if (!t) {
        return;
      }
      t.job = job;
      if ((job.state === "done" || job.state === "error" || job.state === "cancelled") && t.doneAt == null) {
        t.doneAt = Date.now();
      }
      send();
    });
    // A new playback Dolphin counts its requests from 1 again.
    const offClosed = dolphinService.onEvent(DolphinEventType.CLOSED, (event) => {
      if (
        event.dolphinType === DolphinLaunchType.PLAYBACK &&
        "instanceId" in event &&
        event.instanceId === "playback"
      ) {
        handled = new Set();
        shownSeq = 0;
      }
    });
    return () => {
      alive = false;
      clearInterval(timer);
      offStatus();
      offProgress();
      offClosed();
    };
  }, [dolphinService]);
};

async function sendOverlay(dolphinService: DolphinService, overlay: PlaybackOverlayFile) {
  try {
    await dolphinService.setPlaybackOverlay(overlay);
  } catch {
    // (No playback Dolphin, or it closed meanwhile: the next one gets it when it starts.)
  }
}
