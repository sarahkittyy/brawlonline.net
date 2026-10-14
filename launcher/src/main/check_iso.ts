import { IsoValidity } from "@common/types";
import { app } from "electron";
import electronLog from "electron-log";
import path from "path";
import { throttleProgress } from "utils/copy_file";

import { ipc_isoVerificationProgressEvent } from "./ipc";
import { verifyIsoCached } from "./verify_iso";

const log = electronLog.scope("main/check_iso");

const running = new Map<string, Promise<IsoValidity>>();

/**
 * Verifies `isoPath` (cached on disk), reporting the hashing progress to the renderer.
 * Callers asking about the same path while it is being checked share that check, so
 * the Settings page and the game-asset extraction never hash the image twice.
 */
export function checkIso(isoPath: string): Promise<IsoValidity> {
  const existing = running.get(isoPath);
  if (existing) {
    return existing;
  }
  const check = runCheck(isoPath).finally(() => running.delete(isoPath));
  running.set(isoPath, check);
  return check;
}

async function runCheck(isoPath: string): Promise<IsoValidity> {
  try {
    const cacheFile = path.join(app.getPath("userData"), "iso-verification.json");
    // Hashing the 8.5 GB image takes ~25 s: report it (a cache hit reports nothing).
    let hashed = false;
    const onProgress = throttleProgress((current, total) => {
      hashed = true;
      ipc_isoVerificationProgressEvent.main!.trigger({ path: isoPath, current, total }).catch(log.warn);
    });
    const started = Date.now();
    const result = await verifyIsoCached(isoPath, cacheFile, onProgress);
    const took = hashed ? ` in ${((Date.now() - started) / 1000).toFixed(1)} s` : "";
    const line = `ISO check of ${isoPath}: ${result.valid}${took}; ${result.detail}`;
    // A rejected image is logged as a warning, which a release's log keeps.
    if (result.valid === IsoValidity.VALID) {
      log.info(line);
    } else {
      log.warn(line);
    }
    return result.valid;
  } catch (err) {
    // Reading the file failed (missing, access denied, locked, ...): the player sees "will not work".
    const code = (err as NodeJS.ErrnoException)?.code;
    log.warn(`ISO check of ${isoPath} failed${code ? ` (${code})` : ""}, treating the image as invalid:`, err);
    return IsoValidity.INVALID;
  }
}
