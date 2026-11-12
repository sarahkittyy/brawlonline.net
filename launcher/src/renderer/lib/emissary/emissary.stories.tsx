import type { EmissaryState } from "@dolphin/emissary/types";

import { EmissarySettingView } from "@/pages/settings/addons_settings/emissary_setting_view";
import { PlayProjectPlusWithView } from "@/pages/settings/game_settings/play_project_plus_with_view";

export default {
  title: "lib/EmissaryRecomp",
};

const noop = () => undefined;
const latest = {
  version: "v0.6.0",
  assetName: "BrawlRecompiled-v0.6.0-Windows.zip",
  url: "https://github.com/kalomaze/EmissaryRecomp/releases/download/v0.6.0/BrawlRecompiled-v0.6.0-Windows.zip",
  size: 208_641_617,
  sha256: null,
};
const base: EmissaryState = {
  status: "notInstalled",
  folder: "C:\\Users\\you\\AppData\\Roaming\\Brawl Online\\netplay\\emissary-recomp",
  supported: true,
  installed: null,
  gameFiles: null,
  gameFilesCurrent: false,
  discReady: true,
  latest,
  latestError: null,
  upToDate: false,
  downloadBytes: latest.size,
  diskBytes: 7_759_011_557,
  staged: null,
  job: null,
  running: false,
  error: null,
};
const installed = (version: string) => ({
  version,
  programBytes: 452_000_000,
  installedAt: "2026-11-12T20:00:00.000Z",
  playsOnline: true,
});
const files = { bytes: 7_296_000_000 };

const states: Record<string, EmissaryState> = {
  unavailable: { ...base, status: "unavailable", supported: false, latest: null, downloadBytes: 0 },
  needsDisc: { ...base, status: "needsDisc", discReady: false },
  notInstalled: base,
  downloading: {
    ...base,
    status: "downloading",
    job: { kind: "install", phase: "download", current: 86_000_000, total: latest.size },
  },
  unpacking: { ...base, status: "downloading", job: { kind: "install", phase: "unpack", current: 0, total: 0 } },
  preparingFiles: {
    ...base,
    status: "preparingFiles",
    installed: installed("v0.6.0"),
    job: { kind: "gameFiles", phase: "gameFiles", current: 37, total: 100 },
  },
  needsFiles: {
    ...base,
    status: "needsFiles",
    installed: installed("v0.6.0"),
    upToDate: true,
    downloadBytes: 0,
    diskBytes: 7_300_000_000,
  },
  ready: {
    ...base,
    status: "ready",
    installed: installed("v0.6.0"),
    gameFiles: files,
    gameFilesCurrent: true,
    upToDate: true,
    downloadBytes: 0,
    diskBytes: 0,
  },
  updateAvailable: {
    ...base,
    status: "updateAvailable",
    installed: installed("v0.5.3"),
    gameFiles: files,
    gameFilesCurrent: true,
    diskBytes: 0,
  },
  staged: {
    ...base,
    status: "running",
    running: true,
    installed: installed("v0.6.0"),
    gameFiles: files,
    gameFilesCurrent: true,
    staged: "v0.6.1",
    upToDate: true,
    downloadBytes: 0,
    diskBytes: 0,
  },
  offlineOnly: {
    ...base,
    status: "ready",
    installed: { ...installed("v0.6.0"), playsOnline: false },
    gameFiles: files,
    gameFilesCurrent: true,
    upToDate: true,
    downloadBytes: 0,
    diskBytes: 0,
  },
  error: {
    ...base,
    error: "Not enough space on drive C: for EmissaryRecomp: 7.6 GB needed, 2.1 GB free.",
  },
};

const card = (state: EmissaryState) => () =>
  (
    <div style={{ width: 640, padding: 16 }}>
      <EmissarySettingView
        state={state}
        autoUpdate={true}
        onGet={noop}
        onCancel={noop}
        onRemove={noop}
        onOpenFolder={noop}
        onOpenRepo={noop}
        onChooseDisc={noop}
        onAutoUpdate={noop}
      />
    </div>
  );

export const CardUnavailable = card(states.unavailable);
export const CardNeedsDisc = card(states.needsDisc);
export const CardNotInstalled = card(states.notInstalled);
export const CardDownloading = card(states.downloading);
export const CardUnpacking = card(states.unpacking);
export const CardPreparingFiles = card(states.preparingFiles);
export const CardNeedsFiles = card(states.needsFiles);
export const CardReady = card(states.ready);
export const CardUpdateAvailable = card(states.updateAvailable);
export const CardUpdateStagedWhilePlaying = card(states.staged);
export const CardError = card(states.error);
export const CardReleaseWithoutOnline = card(states.offlineOnly);

const choice =
  (value: "dolphin" | "emissary", state: EmissaryState | null, disabled = false) =>
  () =>
    (
      <div style={{ width: 640, padding: 16 }}>
        <PlayProjectPlusWithView
          value={value}
          emissary={state}
          disabled={disabled}
          disabledReason="Close Dolphin to change this setting"
          onChange={noop}
          onSetUp={noop}
        />
      </div>
    );

export const PlayWithDolphinNotInstalled = choice("dolphin", states.notInstalled);
export const PlayWithEmissaryReady = choice("emissary", states.ready);
export const PlayWithNeedsDisc = choice("dolphin", states.needsDisc);
export const PlayWithUnavailable = choice("dolphin", states.unavailable);
export const PlayWithReleaseWithoutOnline = choice("emissary", states.offlineOnly);
export const PlayWithWhileGameOpen = choice("emissary", states.ready, true);
