import type { Progress, ReplayService } from "@replays/types";
import throttle from "lodash/throttle";

import { getReplayPresenter } from "@/lib/hooks/use_replays";
import type { AuthService } from "@/services/auth/types";

import { clearUserData, refreshUserData, useAccount } from "../lib/hooks/use_account";
import { useAppStore } from "../lib/hooks/use_app_store";
import type { Services } from "../services/types";
import { installDolphinListeners } from "./install_dolphin_listeners";
import { installSettingsChangeListeners } from "./install_settings_change_listeners";

export function installAppListeners(services: Services) {
  const { authService, notificationService, backendService, dolphinService, replayService } = services;

  authService.onUserChange((user) => {
    useAccount.getState().setUser(user);

    // Refresh the play key
    if (user) {
      void refreshUserData(backendService);
    } else {
      // We've logged out so clear any pending requests for user data.
      clearUserData();
    }
  });

  // Subscribe to multi-account changes
  const multiAccountService = authService.getMultiAccountService();
  multiAccountService.onAccountsChange(({ accounts, activeId }) => {
    useAccount.getState().setAccounts(accounts);
    useAccount.getState().setActiveAccountId(activeId);
  });

  window.electron.common.onAppUpdateReady(() => {
    useAppStore.getState().setUpdateReady(true);
  });

  window.electron.common.onAppUpdateDownloadProgress((progress) => {
    useAppStore.getState().setUpdateDownloadProgress(progress);
  });

  window.electron.common.onAppUpdateFound((version) => {
    useAppStore.getState().setUpdateVersion(version);
  });

  // Track online/offline status
  window.addEventListener("online", () => {
    useAppStore.getState().setIsOnline(true);
  });
  window.addEventListener("offline", () => {
    useAppStore.getState().setIsOnline(false);
  });

  installDolphinListeners({ dolphinService, notificationService });
  installReplayListeners({ replayService, authService });
  installSettingsChangeListeners({ replayService, authService });
}

export function installReplayListeners({
  replayService,
  authService,
}: {
  replayService: ReplayService;
  authService: AuthService;
}) {
  const replayPresenter = getReplayPresenter(replayService, authService);

  const updateProgress = (progress: Progress | undefined) => replayPresenter.updateProgress(progress);
  const throttledUpdateProgress = throttle(updateProgress, 50);
  replayService.onReplayLoadProgressUpdate(throttledUpdateProgress);
}
