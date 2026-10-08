import type { DolphinSetupPhase } from "@dolphin/types";
import { DolphinLaunchType } from "@dolphin/types";
import { unstable_batchedUpdates } from "react-dom";
import { create } from "zustand";

export const enum DolphinStatus {
  UNKNOWN = "UNKNOWN",
  READY = "READY",
  DOWNLOADING = "DOWNLOADING",
}

export type DolphinProgress = { current: number; total: number; phase?: DolphinSetupPhase };

export const useDolphinStore = create(() => ({
  netplayStatus: DolphinStatus.UNKNOWN,
  playbackStatus: DolphinStatus.UNKNOWN,
  netplayOpened: false,
  playbackOpened: false,
  /** The running set-up step's progress, shown on the Play button. */
  netplayDownloadProgress: undefined as DolphinProgress | undefined,
  netplayDolphinVersion: undefined as string | undefined,
  playbackDolphinVersion: undefined as string | undefined,
}));

export const setDolphinOpened = (dolphinType: DolphinLaunchType, isOpened = true) => {
  switch (dolphinType) {
    case DolphinLaunchType.NETPLAY:
      useDolphinStore.setState({ netplayOpened: isOpened });
      break;
    case DolphinLaunchType.PLAYBACK:
      useDolphinStore.setState({ playbackOpened: isOpened });
      break;
  }
};

export const setDolphinStatus = (dolphinType: DolphinLaunchType, status: DolphinStatus) => {
  switch (dolphinType) {
    case DolphinLaunchType.NETPLAY:
      useDolphinStore.setState({ netplayStatus: status });
      break;
    case DolphinLaunchType.PLAYBACK:
      useDolphinStore.setState({ playbackStatus: status });
      break;
  }
};

export const setDolphinVersion = (dolphinVersion: string | undefined, dolphinType: DolphinLaunchType) => {
  switch (dolphinType) {
    case DolphinLaunchType.NETPLAY:
      useDolphinStore.setState({ netplayDolphinVersion: dolphinVersion });
      break;
    case DolphinLaunchType.PLAYBACK:
      useDolphinStore.setState({ playbackDolphinVersion: dolphinVersion });
      break;
  }
};

export const updateNetplayDownloadProgress = (progress: DolphinProgress | undefined) => {
  unstable_batchedUpdates(() => {
    useDolphinStore.setState({ netplayDownloadProgress: progress });
  });
};
