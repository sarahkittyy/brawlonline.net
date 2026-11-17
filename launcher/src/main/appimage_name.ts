import log from "electron-log";
import { renameSync } from "node:fs";
import path from "path";

/** The part of electron-updater's AppImageUpdater that an update's install goes through. */
type AppImageInstaller = {
  readonly installerPath: string | null;
  doInstall(options: unknown): boolean;
};

/**
 * Makes an AppImage update keep the file's name. electron-updater's AppImageUpdater moves the new
 * version over the running AppImage only when its name holds no version; otherwise it saves it under
 * the new version's name (Brawl-Online-0.2.5-x86_64.AppImage) and deletes the old file, which broke
 * every start menu entry and Steam shortcut to it at each update (the website's button linked the
 * versioned file until it linked BrawlOnline.AppImage). Giving the downloaded file the running
 * AppImage's name first makes electron-updater replace that file in place: same path, new version.
 */
export function keepAppImageName(updater: AppImageInstaller, env: NodeJS.ProcessEnv = process.env): void {
  const install = updater.doInstall;
  updater.doInstall = function (this: AppImageInstaller, options: unknown): boolean {
    const running = env.APPIMAGE;
    const downloaded = this.installerPath;
    if (!running || !downloaded || path.basename(downloaded) === path.basename(running)) {
      return install.call(this, options);
    }
    const renamed = path.join(path.dirname(downloaded), path.basename(running));
    try {
      renameSync(downloaded, renamed);
    } catch (err) {
      log.warn(`Could not give the update the AppImage's name ${path.basename(running)}:`, err);
      return install.call(this, options);
    }
    log.warn(`Installing the update as ${running}`);
    // installerPath is a getter on BaseUpdater (the downloaded file); this one answers for the install.
    Object.defineProperty(this, "installerPath", { value: renamed, configurable: true });
    try {
      return install.call(this, options);
    } finally {
      delete (this as { installerPath?: string | null }).installerPath;
    }
  };
}
