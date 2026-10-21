/**
 * Play's popup when the SD card is on another Project+ release than the launcher's (bug report #74:
 * every game desyncs). "Download" replaces the card with the launcher's release (the old one is kept
 * next to it) and starts the game as Play would have.
 */
import { pplusReleaseLabel } from "@common/pplus_releases";
import { DolphinLaunchType } from "@dolphin/types";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import LinearProgress from "@mui/material/LinearProgress";
import log from "electron-log";
import { create } from "zustand";

import { setupPhaseLabel } from "@/app/header/play_button/play_button";
import { setDolphinOpened, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";

import { PPlusBuildDialogMessages as Messages } from "./pplus_build_dialog.messages";
import { formatBytes } from "./replay_version_dialog";

type View = { kind: "ask" } | { kind: "working" } | { kind: "error"; message: string };

type Store = {
  /** The card's release and the launcher's, with how Play was started; null when closed. */
  build: { cardRelease: string; release: string; bootGame?: boolean } | null;
  view: View;
};

export const usePPlusBuildDialog = create<Store>(() => ({ build: null, view: { kind: "ask" } }));

export const showWrongPPlusBuild = (build: NonNullable<Store["build"]>) =>
  usePPlusBuildDialog.setState({ build, view: { kind: "ask" } });

const close = () => usePPlusBuildDialog.setState({ build: null, view: { kind: "ask" } });

/** Replaces the card, then starts the game as Play would have. */
async function downloadAndPlay() {
  const { build } = usePPlusBuildDialog.getState();
  if (!build) {
    return;
  }
  usePPlusBuildDialog.setState({ view: { kind: "working" } });
  try {
    await window.electron.dolphin.replaceProjectPlusCard();
    const result = await window.electron.dolphin.launchNetplayDolphin({ bootGame: build.bootGame });
    if (result.status === "wrongPPlusBuild") {
      usePPlusBuildDialog.setState({
        view: { kind: "error", message: Messages.stillWrong(pplusReleaseLabel(result.cardRelease)) },
      });
      return;
    }
    setDolphinOpened(DolphinLaunchType.NETPLAY);
    close();
  } catch (err) {
    log.error(err);
    usePPlusBuildDialog.setState({
      view: { kind: "error", message: Messages.error(err instanceof Error ? err.message : String(err)) },
    });
  }
}

export const PPlusBuildDialog = () => {
  const build = usePPlusBuildDialog((s) => s.build);
  const view = usePPlusBuildDialog((s) => s.view);
  const progress = useDolphinStore((s) => s.netplayDownloadProgress);

  if (!build) {
    return null;
  }
  const cardRelease = pplusReleaseLabel(build.cardRelease);
  const release = pplusReleaseLabel(build.release);
  const working = view.kind === "working";
  const percent = progress && progress.total > 0 ? Math.min(100, (progress.current / progress.total) * 100) : 0;

  return (
    <Dialog
      open={true}
      onClose={(_, reason) => {
        if (!working && reason !== "backdropClick") {
          close();
        }
      }}
      maxWidth="sm"
      fullWidth={true}
    >
      <DialogTitle style={{ fontSize: 26, fontWeight: 700 }}>
        {view.kind === "error" ? Messages.errorTitle(release) : Messages.title()}
      </DialogTitle>
      <DialogContent>
        {view.kind === "error" ? (
          <p style={{ marginTop: 0 }}>{view.message}</p>
        ) : (
          <>
            <p style={{ marginTop: 0, fontSize: 18 }}>{Messages.explain(cardRelease, release)}</p>
            <p style={{ opacity: 0.75 }}>{Messages.kept(cardRelease)}</p>
          </>
        )}
        {working && (
          <div style={{ marginTop: 16 }}>
            <div style={{ display: "flex", opacity: 0.85 }}>
              <span>{setupPhaseLabel(progress?.phase)}</span>
              {progress?.phase === "downloadProjectPlus" && progress.total > 0 && (
                <span style={{ marginLeft: "auto", fontVariantNumeric: "tabular-nums" }}>
                  {formatBytes(progress.current)} / {formatBytes(progress.total)}
                </span>
              )}
            </div>
            <LinearProgress
              style={{ marginTop: 8 }}
              variant={progress && progress.total > 0 ? "determinate" : "indeterminate"}
              value={percent}
            />
          </div>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={close} disabled={working}>
          {Messages.cancel()}
        </Button>
        <Button variant="contained" color="primary" onClick={() => void downloadAndPlay()} disabled={working}>
          {view.kind === "error" ? Messages.retry() : Messages.download(release)}
        </Button>
      </DialogActions>
    </Dialog>
  );
};

/**
 * Test mode only (bootstrap.testMode): `window.__brawlTest.pplusBuildDialog(build, view)` shows the
 * popup, for screenshots.
 */
export function installPPlusBuildDialogPreview() {
  if (!window.electron.bootstrap.testMode) {
    return;
  }
  const w = window as unknown as { __brawlTest?: Record<string, unknown> };
  w.__brawlTest = {
    ...w.__brawlTest,
    pplusBuildDialog: (build: Store["build"], view?: View) =>
      usePPlusBuildDialog.setState({ build, view: view ?? { kind: "ask" } }),
  };
}
