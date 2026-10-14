import setupAccountsIpc from "@accounts/setup";
import { DolphinManager } from "@dolphin/manager";
import setupModsIpc from "@dolphin/mods/setup";
import setupDolphinIpc from "@dolphin/setup";
import setupGameAssetsIpc from "@game_assets/setup";
import setupReplaysIpc from "@replays/setup";
import { SettingsManager } from "@settings/settings_manager";
import setupSettingsIpc from "@settings/setup";
import setupSocialIpc from "@social/setup";
import setupSpectateIpc from "@spectate/setup";

import { AppUpdater } from "./app_updater";
import { BrowserWindowManager } from "./browser_window_manager";
import { checkIso } from "./check_iso";
import type { ConfigFlags } from "./flags/flags";
import setupMainIpc from "./setup";

export function installModules(flags: ConfigFlags) {
  const settingsManager = new SettingsManager();
  const appUpdater = new AppUpdater(settingsManager);
  const dolphinManager = new DolphinManager(settingsManager);
  dolphinManager.setLaunchGate(() => appUpdater.assertUpToDate());
  setupDolphinIpc({ dolphinManager });
  const { accountsManager, serviceUrls } = setupAccountsIpc();
  setupSocialIpc({ accountsManager, accountsApi: serviceUrls.accountsApi, dolphinManager });
  setupSpectateIpc({ accountsManager, accountsApi: serviceUrls.accountsApi, dolphinManager, settingsManager });
  setupReplaysIpc();
  setupSettingsIpc({ settingsManager, dolphinManager });
  setupModsIpc({ settingsManager, dolphinManager, accountsManager });
  setupGameAssetsIpc({ settingsManager, dolphinManager, checkIso });
  const browserWindowManager = new BrowserWindowManager();
  setupMainIpc({ dolphinManager, settingsManager, flags, browserWindowManager, appUpdater, accountsManager });
  return { dolphinManager, settingsManager, browserWindowManager, appUpdater };
}
