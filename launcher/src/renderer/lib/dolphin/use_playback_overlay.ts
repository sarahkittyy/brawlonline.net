/**
 * The playback Dolphin's seek bar, the launcher's side (`<comm>.overlay.json`, Core/Rollback/ReplayPlayback.h):
 * the launcher's colour for the bar's played part (Sarah's board, 2026-11-20: "the played part should be your
 * launcher's color"), and clip mode's Render (w/ Music) / Render (No Music): Dolphin asks (status
 * `renderRequests`), this queues the render as the Highlights tab's Render does (the clip is a Manual Clip by
 * then), and the render's progress shows in the game, at its top right.
 *
 * Mounted once for the whole window (app/create.tsx).
 */
import type {
  DolphinService,
  PlaybackOverlayFile,
  PlaybackOverlayToast,
  RenderJobInfo,
  ReplayPlaybackStatus,
} from "@dolphin/types";
import { DolphinEventType, DolphinLaunchType } from "@dolphin/types";
import type { HighlightPlayer, ManualClip } from "@replays/highlights/types";
import { useEffect, useRef } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useSettingsStore } from "@/lib/hooks/use_settings";
import { HighlightListMessages as Messages } from "@/pages/replays/replay_file_stats/highlights/highlight_list.messages";
import { renderStatusText } from "@/pages/replays/replay_file_stats/highlights/render_jobs";
import { clipVideoName, replayDateOf } from "@/pages/replays/replay_file_stats/highlights/render_names";
import { useServices } from "@/services";

type RenderAsk = NonNullable<ReplayPlaybackStatus["renderRequests"]>[number];

/** How often the launcher's colour is looked at (the theme can change while Dolphin is open). */
const ACCENT_POLL_MS = 1000;
/** How long a render's clip may take to be kept as a Manual Clip (a spectated game is copied first). */
const CLIP_WAIT_MS = 20_000;
/** How long a finished render stays in the game. */
const DONE_SHOWN_MS = 6000;

/** "#rrggbb" of a CSS colour (as the page computes it), or null. */
export function cssColorToHex(css: string): string | null {
  const m = /^rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/i.exec(css.trim());
  if (m) {
    return `#${[m[1], m[2], m[3]].map((v) => Math.min(255, Number(v)).toString(16).padStart(2, "0")).join("")}`;
  }
  const hex = /^#([0-9a-f]{6})$/i.exec(css.trim());
  if (hex) {
    return `#${hex[1].toLowerCase()}`;
  }
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i.exec(css.trim());
  return short ? `#${short[1]}${short[1]}${short[2]}${short[2]}${short[3]}${short[3]}`.toLowerCase() : null;
}

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

/** "0:20" */
export function clipLength(startFrame: number, endFrame: number): string {
  const seconds = Math.max(0, Math.round((endFrame - startFrame) / 60));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}

/** The Manual Clip Dolphin's render request is of (a spectated game's: its copy, `source` the stream's file). */
export function findClip(clips: readonly ManualClip[], ask: RenderAsk): ManualClip | null {
  return (
    clips.find(
      (c) =>
        c.startFrame === ask.startFrame &&
        c.endFrame === ask.endFrame &&
        (c.path === ask.path || c.source === ask.path),
    ) ?? null
  );
}

/** A render job as the game shows it. */
export function toastOf(job: RenderJobInfo, label: string): PlaybackOverlayToast {
  const state: PlaybackOverlayToast["state"] =
    job.state === "done"
      ? "done"
      : job.state === "error" || job.state === "cancelled"
      ? "error"
      : job.state === "rendering"
      ? "rendering"
      : "queued";
  return { label, text: renderStatusText(job), progress: job.progress, state };
}

type Tracked = { label: string; job: RenderJobInfo | null; doneAt: number | null };

export const usePlaybackOverlay = () => {
  const { dolphinService } = useServices();
  const { renderHighlights } = useDolphinActions(dolphinService);
  const renderRef = useRef(renderHighlights);
  renderRef.current = renderHighlights;

  useEffect(() => {
    let accent: string | null = null;
    let lastSent = "";
    // Renders asked for in this playback Dolphin's run (its seqs start at 1 again in the next one).
    let handled = new Set<number>();
    const tracked = new Map<string, Tracked>();
    let alive = true;

    const send = () => {
      const now = Date.now();
      const toasts: PlaybackOverlayToast[] = [];
      for (const [id, t] of tracked) {
        if (t.doneAt != null && now - t.doneAt >= DONE_SHOWN_MS) {
          tracked.delete(id);
          continue;
        }
        if (t.job) {
          toasts.push(toastOf(t.job, t.label));
        }
      }
      const overlay: PlaybackOverlayFile = { ...(accent ? { accent } : {}), toasts: toasts.slice(-4) };
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
      // The clip as the launcher keeps it (manual_clips.ts): its file (a spectated game's copy) and players.
      const until = Date.now() + CLIP_WAIT_MS;
      let clip = findClip(useSettingsStore.getState().settings.manualClips, ask);
      while (!clip && alive && Date.now() < until) {
        await new Promise((resolve) => setTimeout(resolve, 250));
        clip = findClip(useSettingsStore.getState().settings.manualClips, ask);
      }
      const path = clip?.path ?? ask.path;
      const players = new Map<number, HighlightPlayer>((clip?.players ?? []).map((p) => [p.port, p]));
      const name = clip
        ? clipVideoName(
            { kind: "manual", startFrame: ask.startFrame, endFrame: ask.endFrame, savedAt: clip.savedAt },
            players,
            path,
          )
        : [replayDateOf(path), "Clip"].filter(Boolean).join(" ");
      const label = Messages.manualClip(clipLength(ask.startFrame, ask.endFrame));
      const jobs = await renderRef.current({
        items: [{ path, startFrame: ask.startFrame, endFrame: ask.endFrame }],
        names: [name],
        music: ask.music,
        combo: false,
      });
      for (const job of jobs ?? []) {
        tracked.set(job.jobId, { label, job, doneAt: null });
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
