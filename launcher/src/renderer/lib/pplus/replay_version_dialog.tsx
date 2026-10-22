/**
 * The one dialog for replays from another Project+ release (docs/design/pplus-versions.md): asks
 * before that release's files are downloaded (or taken from the player's own Project+ folder),
 * shows the download and the card being made, says why it failed, or that this launcher can't play
 * that release at all. Promise-based: play, highlights and (later) rendering await the answer.
 */
import { formatBytes } from "@common/disk_space";
import { pplusReleaseLabel } from "@common/pplus_releases";
import { DolphinEventType } from "@dolphin/types";
import CheckCircleIcon from "@mui/icons-material/CheckCircle";
import CloudDownloadIcon from "@mui/icons-material/CloudDownload";
import SdStorageIcon from "@mui/icons-material/SdStorage";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import LinearProgress from "@mui/material/LinearProgress";
import { useEffect, useState } from "react";
import { create } from "zustand";

import { ReplayVersionDialogMessages as Messages } from "./replay_version_dialog.messages";

export type VersionPurpose = "play" | "render";
export type AskAnswer =
  | { action: "download" }
  | { action: "import"; path: string }
  | { action: "current" }
  | { action: "cancel" };
export type ErrorAnswer = "retry" | "current" | "cancel";

type View =
  | { kind: "ask"; release: string; current: string; downloadBytes: number; purpose: VersionPurpose }
  | { kind: "working"; release: string; current: string; purpose: VersionPurpose; importing: boolean }
  | { kind: "error"; release: string; current: string; purpose: VersionPurpose; code: string; message: string }
  | { kind: "unplayable"; release: string; current: string; purpose: VersionPurpose };

type Store = {
  view: View | null;
  resolve: ((answer: any) => void) | null;
  show: <T>(view: View) => Promise<T>;
  setView: (view: View | null) => void;
};

export const useReplayVersionDialog = create<Store>((set, get) => ({
  view: null,
  resolve: null,
  show: <T,>(view: View) =>
    new Promise<T>((resolve) => {
      get().resolve?.(view.kind === "ask" ? { action: "cancel" } : "cancel");
      set({ view, resolve });
    }),
  setView: (view) => set({ view, resolve: null }),
}));

const answer = (value: unknown) => {
  const { resolve } = useReplayVersionDialog.getState();
  useReplayVersionDialog.setState({ resolve: null });
  resolve?.(value);
};

export const askForRelease = (v: Omit<Extract<View, { kind: "ask" }>, "kind">) =>
  useReplayVersionDialog.getState().show<AskAnswer>({ kind: "ask", ...v });
export const showReleaseError = (v: Omit<Extract<View, { kind: "error" }>, "kind">) =>
  useReplayVersionDialog.getState().show<ErrorAnswer>({ kind: "error", ...v });
export const showUnplayable = (v: Omit<Extract<View, { kind: "unplayable" }>, "kind">) =>
  useReplayVersionDialog.getState().show<ErrorAnswer>({ kind: "unplayable", ...v });
export const showWorking = (v: Omit<Extract<View, { kind: "working" }>, "kind">) =>
  useReplayVersionDialog.getState().setView({ kind: "working", ...v });
export const closeReleaseDialog = () => useReplayVersionDialog.getState().setView(null);

export { formatBytes };

type Progress = { phase: string; current: number; total: number } | null;

const progressValue = (p: Progress) => (p && p.total > 0 ? Math.min(100, (p.current / p.total) * 100) : 0);

const Step = ({
  done,
  active,
  label,
  extra,
  progress,
}: {
  done: boolean;
  active: boolean;
  label: string;
  extra?: string;
  progress: Progress;
}) => (
  <div style={{ margin: "12px 0" }}>
    <div style={{ display: "flex", alignItems: "center", gap: 8, opacity: active || done ? 1 : 0.5 }}>
      {done ? <CheckCircleIcon fontSize="small" color="primary" /> : <span style={{ width: 20 }} />}
      <span style={{ fontWeight: active ? 600 : 400 }}>{label}</span>
      {extra && <span style={{ marginLeft: "auto", fontVariantNumeric: "tabular-nums", opacity: 0.8 }}>{extra}</span>}
    </div>
    {active && (
      <LinearProgress
        style={{ marginTop: 8, marginLeft: 28 }}
        variant={progress && progress.total > 0 ? "determinate" : "indeterminate"}
        value={progressValue(progress)}
      />
    )}
  </div>
);

export const ReplayVersionDialog = () => {
  const view = useReplayVersionDialog((s) => s.view);
  const [progress, setProgress] = useState<{ phase: string; current: number; total: number } | null>(null);

  useEffect(() => {
    if (view?.kind !== "working") {
      setProgress(null);
      return;
    }
    return window.electron.dolphin.onEvent(DolphinEventType.PPLUS_PREPARE_PROGRESS, (e) => {
      if (e.release === view.release) {
        setProgress({ phase: e.phase, current: e.current, total: e.total });
      }
    });
  }, [view]);

  if (!view) {
    return null;
  }
  const version = pplusReleaseLabel(view.release);
  const current = pplusReleaseLabel(view.current);
  const render = view.purpose === "render";
  const cancel = () => {
    answer(view.kind === "ask" ? { action: "cancel" } : "cancel");
    closeReleaseDialog();
  };

  if (view.kind === "ask") {
    const chooseFolder = async () => {
      const result = await window.electron.common.showOpenDialog({
        title: Messages.chooseFolderTitle(version),
        properties: ["openDirectory"],
      });
      if (!result.canceled && result.filePaths.length) {
        answer({ action: "import", path: result.filePaths[0] });
      }
    };
    return (
      <Dialog open={true} onClose={cancel} maxWidth="sm" fullWidth={true}>
        <DialogTitle>{render ? Messages.titleClip(version) : Messages.titleReplay(version)}</DialogTitle>
        <DialogContent>
          <p style={{ marginTop: 0 }}>
            {render ? Messages.explainRender(version, current) : Messages.explain(version, current)}
          </p>
          <div style={{ display: "flex", alignItems: "center", gap: 8, margin: "6px 0", opacity: 0.85 }}>
            <CloudDownloadIcon fontSize="small" />
            {Messages.download(formatBytes(view.downloadBytes || 51_400_000))}
          </div>
          <div style={{ display: "flex", alignItems: "center", gap: 8, margin: "6px 0", opacity: 0.85 }}>
            <SdStorageIcon fontSize="small" />
            {Messages.diskSpace(version)}
          </div>
          <div style={{ marginTop: 8 }}>
            <Button size="small" onClick={() => void chooseFolder()} style={{ paddingLeft: 0, textTransform: "none" }}>
              {Messages.haveIt(version)}
            </Button>
          </div>
        </DialogContent>
        <DialogActions>
          <Button onClick={cancel}>{Messages.cancel()}</Button>
          {!render && (
            <Button color="secondary" onClick={() => answer({ action: "current" })}>
              {Messages.playAnyway(current)}
            </Button>
          )}
          <Button variant="contained" color="primary" onClick={() => answer({ action: "download" })}>
            {render ? Messages.downloadAndRender() : Messages.downloadAndPlay()}
          </Button>
        </DialogActions>
      </Dialog>
    );
  }

  if (view.kind === "working") {
    const files = progress?.phase === "card";
    const downloading = !files;
    return (
      <Dialog open={true} maxWidth="sm" fullWidth={true}>
        <DialogTitle>{Messages.workingTitle(version)}</DialogTitle>
        <DialogContent>
          <Step
            progress={progress}
            done={files}
            active={downloading}
            label={view.importing ? Messages.stepImport() : Messages.stepFiles()}
            extra={
              downloading && progress?.phase === "download" && progress.total > 0
                ? `${formatBytes(progress.current)} / ${formatBytes(progress.total)}`
                : undefined
            }
          />
          <Step
            progress={progress}
            done={false}
            active={files}
            label={Messages.stepCard()}
            extra={files && progress && progress.total > 0 ? `${Math.round(progressValue(progress))}%` : undefined}
          />
        </DialogContent>
      </Dialog>
    );
  }

  if (view.kind === "error") {
    const text =
      view.code === "offline"
        ? Messages.errorOffline()
        : view.code === "damaged"
        ? Messages.errorDamaged()
        : view.code === "card-unknown"
        ? Messages.errorCardUnknown(view.message)
        : Messages.errorOther(view.message);
    return (
      <Dialog open={true} onClose={cancel} maxWidth="sm" fullWidth={true}>
        <DialogTitle>{Messages.errorTitle(version)}</DialogTitle>
        <DialogContent>
          <p style={{ marginTop: 0 }}>{text}</p>
        </DialogContent>
        <DialogActions>
          <Button onClick={cancel}>{Messages.cancel()}</Button>
          {!render && (
            <Button color="secondary" onClick={() => answer("current")}>
              {Messages.playAnyway(current)}
            </Button>
          )}
          <Button variant="contained" color="primary" onClick={() => answer("retry")}>
            {Messages.retry()}
          </Button>
        </DialogActions>
      </Dialog>
    );
  }

  return (
    <Dialog open={true} onClose={cancel} maxWidth="sm" fullWidth={true}>
      <DialogTitle>{Messages.unplayableTitle(version)}</DialogTitle>
      <DialogContent>
        <p style={{ marginTop: 0 }}>{render ? Messages.unplayableRender(version) : Messages.unplayable(version)}</p>
      </DialogContent>
      <DialogActions>
        <Button onClick={cancel}>{render ? Messages.ok() : Messages.cancel()}</Button>
        {!render && (
          <Button color="secondary" onClick={() => answer("current")}>
            {Messages.playAnyway(current)}
          </Button>
        )}
      </DialogActions>
    </Dialog>
  );
};

/**
 * Test mode only (bootstrap.testMode): `window.__brawlTest.replayVersionDialog(view)` shows a state
 * of the dialog, for screenshots of states that are hard to reach (rendering, errors).
 */
export function installReplayVersionDialogPreview() {
  if (!window.electron.bootstrap.testMode) {
    return;
  }
  const w = window as unknown as { __brawlTest?: Record<string, unknown> };
  w.__brawlTest = {
    ...w.__brawlTest,
    replayVersionDialog: (view: View | null) => {
      if (view) {
        void useReplayVersionDialog.getState().show(view);
      } else {
        closeReleaseDialog();
      }
    },
  };
}
