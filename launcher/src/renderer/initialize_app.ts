import { isDiskSpaceError } from "@common/disk_space";
import { PRODUCT_NAME } from "@common/product";
import { DolphinLaunchType } from "@dolphin/types";
import log from "electron-log";

import { generateQuickStartSteps } from "@/pages/quick_start/use_quick_start";
import type { AuthUser } from "@/services/auth/types";
import type { Services } from "@/services/types";

import { InitializeAppMessages as Messages } from "./initialize_app.messages";
import type { UserData } from "./services/backend/types";

/**
 * How long the start waits for the account server before the launcher opens without its answer
 * (playing offline needs none). A server that doesn't answer at all (some Spanish ISPs drop
 * Cloudflare's addresses during football matches) kept the loading screen up for minutes.
 */
const STARTUP_SERVER_WAIT_MS = 5000;

const TIMED_OUT = Symbol("timed out");

/** `promise`'s value, or TIMED_OUT if it hasn't settled by `deadline` (a `Date.now()` time). */
async function settledBy<T>(promise: Promise<T>, deadline: number): Promise<T | typeof TIMED_OUT> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<typeof TIMED_OUT>((resolve) => {
    timer = setTimeout(() => resolve(TIMED_OUT), Math.max(0, deadline - Date.now()));
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

export async function initializeApp(services: Services) {
  const { authService, backendService, dolphinService, notificationService } = services;
  const { showError } = notificationService;

  log.info("Initializing app...");

  const promises: Promise<any>[] = [];

  // If we're logged in, check they have a valid play key
  promises.push(
    (async () => {
      const deadline = Date.now() + STARTUP_SERVER_WAIT_MS;
      let user: AuthUser | undefined = undefined;
      try {
        // Waits at most RESTORE_WAIT_MS (multi_account.service.ts) for the server.
        user = await authService.init();
        // useAccount.getState().setUser(user);
      } catch (err) {
        log.warn(err);
      }

      let userData: UserData | undefined = undefined;
      let serverError = false;
      if (user) {
        // Past the deadline the launcher opens as if the server were down; the error still shows
        // if the request fails later, and the header gets the answer either way (refreshUserData).
        const fetched = backendService.fetchUserData().then(
          (data) => ({ data }),
          () => {
            const reason = !navigator.onLine ? Messages.youAreOffline() : Messages.serversMayBeDown(PRODUCT_NAME);
            const message = `${Messages.failedToCommunicateWithServers(PRODUCT_NAME)} ${reason}`;
            showError(message);
            return null;
          },
        );
        const answer = await settledBy(fetched, deadline);
        if (answer === TIMED_OUT) {
          log.warn(`No answer from the server in ${STARTUP_SERVER_WAIT_MS} ms, starting without the user data`);
        }
        serverError = answer === TIMED_OUT || answer === null;
        userData = answer !== TIMED_OUT && answer !== null ? answer.data : undefined;
      }

      // Generate the quick start steps based on the current state
      generateQuickStartSteps({ user, userData, serverError });
    })(),
  );

  // Download Dolphins if necessary
  [DolphinLaunchType.NETPLAY, DolphinLaunchType.PLAYBACK].map((dolphinType) => {
    void dolphinService.downloadDolphin(dolphinType).catch((err) => {
      log.error(err);
      const dolphinTypeName =
        dolphinType === DolphinLaunchType.NETPLAY ? Messages.netplayDolphin() : Messages.playbackDolphin();
      // A full disk says which drive and how much to free (bug report #91), not "close Dolphin".
      showError(isDiskSpaceError(err) ? err : Messages.failedToInstallDolphin(dolphinTypeName));
    });
  });

  // Check if there is an update to the launcher. Not waited for: its request has no timeout, so an
  // unreachable server held the loading screen until the connection gave up (minutes on Linux),
  // and a found update shows in the header's bar anyway (install_app_listeners.ts).
  void window.electron.common.checkForAppUpdates().catch((err) => {
    log.error(err);
  });

  // Ensure the fonts are loaded to prevent FOUT (Flash of Unstyled Text)
  promises.push(document.fonts.ready);

  // Wait for all the promises to complete before returning
  const results = await Promise.allSettled(promises);
  results
    .filter((result): result is PromiseRejectedResult => result.status === "rejected")
    .forEach((result) => {
      log.error(result.reason);
    });

  // Show the update result after awaiting the promises since we load the app
  // via React.Suspense so nothing is visible anyway while the promises are being awaited.
  const updateState = window.electron.bootstrap.updateState;
  if (updateState) {
    if (updateState.status === "succeeded") {
      notificationService.showInfo(Messages.updatedToVersion(PRODUCT_NAME, updateState.version));
    } else {
      showError(Messages.updateFailed(updateState.version));
    }
  }
}
