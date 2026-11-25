import type { SettingsManager } from "@settings/settings_manager";

/**
 * Whether this start is the first of a new launcher version on a computer that ran another one
 * (docs/design/changelog.md): then the page shows the newest changelog once (changelogPending,
 * lib/changelog). `lastRunVersion` is the version that ran before (absent before the release that
 * brought changelogs); a launcher with no settings at all is a new install, which shows none.
 */
export function isLauncherUpdate(lastRunVersion: string | undefined, version: string, freshInstall: boolean): boolean {
  if (lastRunVersion === undefined) {
    return !freshInstall;
  }
  return lastRunVersion !== version;
}

/** Notes the version that runs now, and a pending changelog when it is an update. Before the window opens. */
export async function noteLaunchVersion(settingsManager: SettingsManager, version: string): Promise<void> {
  const { lastRunVersion } = settingsManager.get();
  if (isLauncherUpdate(lastRunVersion, version, settingsManager.freshInstall)) {
    await settingsManager.updateSetting("changelogPending", true);
  }
  await settingsManager.updateSetting("lastRunVersion", version);
}
