import type { ProjectPunchState } from "@dolphin/project_punch/types";

import { PlayButtonsView } from "@/pages/home/play_buttons/play_buttons_view";
import { ProjectPunchButtonView } from "@/pages/home/project_punch_button/project_punch_button";
import { Tabs } from "@/pages/home/tabs/tabs";
import { ProjectPunchSettingView } from "@/pages/settings/training_settings/project_punch_setting_view";

import { ProjectPunchDialogView } from "./project_punch_dialog_view";

export default {
  title: "lib/ProjectPunch",
};

const noop = () => undefined;
const CARD = 2_097_152_000;
const latest = {
  version: "v1.3.1",
  assetName: "ProjectPunch-1.3.1.zip",
  url: "https://github.com/WispSSBM/ProjectPunch/releases/download/v1.3.1/ProjectPunch-1.3.1.zip",
  size: 37_697,
  sha256: null,
};
const base: ProjectPunchState = {
  folder: "C:\\Users\\you\\AppData\\Roaming\\Brawl Online\\netplay\\project-punch",
  cardPath: "C:\\Users\\you\\AppData\\Roaming\\Brawl Online\\netplay\\project-punch\\sd.raw",
  installed: null,
  latest,
  latestError: null,
  needsCard: true,
  upToDate: false,
  downloadBytes: latest.size,
  job: null,
  running: false,
  error: null,
};
const installed = (version: string) => ({
  version,
  cardBytes: CARD,
  pplusRelease: "v3.2.1",
  bootsToTraining: true,
  installedAt: "2026-11-09T21:00:00.000Z",
});
const notInstalled = base;
const downloading: ProjectPunchState = {
  ...base,
  job: { kind: "install", phase: "card", current: 806_000_000, total: CARD },
};
const old: ProjectPunchState = {
  ...base,
  installed: installed("v1.3.0"),
  needsCard: false,
  downloadBytes: latest.size,
};
const current: ProjectPunchState = { ...old, installed: installed("v1.3.1"), upToDate: true, downloadBytes: 0 };
const offline: ProjectPunchState = { ...base, latest: null, latestError: "getaddrinfo ENOTFOUND", downloadBytes: 0 };

const dialog = (state: ProjectPunchState) => () =>
  (
    <ProjectPunchDialogView
      open={true}
      state={state}
      onClose={noop}
      onGet={noop}
      onCancel={noop}
      onPlay={noop}
      onRetry={noop}
      onOpenRepo={noop}
    />
  );

export const DialogNotDownloaded = dialog(notInstalled);
export const DialogDownloading = dialog(downloading);
export const DialogNotLatest = dialog(old);
export const DialogLatest = dialog(current);
export const DialogOffline = dialog(offline);

/** Home's tab row as home_page.tsx lays it out: the lab button at the far right. */
const HomeRow = ({ state }: { state: ProjectPunchState }) => (
  <div style={{ width: 1100, padding: 16, display: "flex", alignItems: "flex-start", gap: 8 }}>
    <Tabs
      value="overview"
      tabs={[
        { id: "overview", label: "Overview" },
        { id: "spectate", label: "Spectate" },
        { id: "mods", label: "Other Games" },
      ]}
    />
    <PlayButtonsView twosQueue={2} twosPlaying={8} onPlay={noop} />
    <ProjectPunchButtonView job={state.job} onClick={noop} />
  </div>
);

export const HomeButton = () => <HomeRow state={current} />;
export const HomeButtonDownloading = () => <HomeRow state={downloading} />;

const setting = (state: ProjectPunchState) => () =>
  (
    <div style={{ width: 640, padding: 16 }}>
      <ProjectPunchSettingView
        state={state}
        onGet={noop}
        onCancel={noop}
        onPlay={noop}
        onRemove={noop}
        onShowCard={noop}
        onOpenRepo={noop}
      />
    </div>
  );

export const SettingsNotInstalled = setting(notInstalled);
export const SettingsDownloading = setting(downloading);
export const SettingsNotLatest = setting(old);
export const SettingsInstalled = setting(current);
