import { copy } from "fs-extra";
import { chmod, chown } from "node:fs/promises";
import os from "os";
import path from "path";

import { mountDmg, unmountDmg } from "./extract_dmg";

/** Bundle name of our Dolphin build (mainline Dolphin's default). */
const DOLPHIN_APP = "Dolphin.app";

export async function installDolphinOnMac({
  assetPath,
  destinationFolder,
  log = console.log,
}: {
  assetPath: string;
  destinationFolder: string;
  log?: (message: string) => void;
}) {
  log(`Extracting to: ${destinationFolder}`);

  const mountPath = await mountDmg(assetPath);
  try {
    const appMountPath = path.join(mountPath, DOLPHIN_APP);
    const destPath = path.join(destinationFolder, DOLPHIN_APP);
    // Use fs-extra's copy() for macOS .app bundles.
    // Replacing this with fs.cp() has previously caused copied apps to fail
    // macOS code-signature validation ("app is damaged" errors).
    await copy(appMountPath, destPath, { recursive: true });
  } catch {
    log("Failed to copy files from DMG");
  } finally {
    await unmountDmg(mountPath);
  }

  try {
    // sometimes permissions aren't set properly after the extraction so we will forcibly set them on install
    const binaryLocation = path.join(destinationFolder, DOLPHIN_APP, "Contents", "MacOS", "Dolphin");
    const userInfo = os.userInfo();

    await Promise.all([
      chmod(path.join(destinationFolder, DOLPHIN_APP), "777"),
      chown(path.join(destinationFolder, DOLPHIN_APP), userInfo.uid, userInfo.gid),
      chmod(binaryLocation, "777"),
      chown(binaryLocation, userInfo.uid, userInfo.gid),
    ]);
  } catch {
    log("Could not chown/chmod Dolphin");
  }
}
