import { chatKeyExpression } from "@common/chat_key";
import { DOLPHIN_ONLINE_DIR } from "@common/product";
import { matchFrames } from "@replays/highlights/match_frames";
import { shell } from "electron";
import log from "electron-log";
import isEqual from "lodash/isEqual";
import { mkdir, readFile } from "node:fs/promises";
import path from "path";
import { fileExists } from "utils/file_exists";

import { PPlusVersionError } from "./install/pplus_versions";
import {
  ipc_cancelRender,
  ipc_cancelRoomLaunch,
  ipc_checkPlayKeyExists,
  ipc_closeNetplayIfIdle,
  ipc_configureDolphin,
  ipc_dolphinEvent,
  ipc_downloadDolphin,
  ipc_findMissingDlls,
  ipc_getChatKey,
  ipc_getDolphinPaths,
  ipc_getPPlusVersions,
  ipc_getRenderJobs,
  ipc_hardResetDolphin,
  ipc_installRosetta,
  ipc_joinRoomInGame,
  ipc_launchNetplayDolphin,
  ipc_moveGameFiles,
  ipc_openDolphinSettingsFolder,
  ipc_playHighlights,
  ipc_preparePPlusRelease,
  ipc_prepareRoomLaunch,
  ipc_removePlayKeyFile,
  ipc_removePPlusRelease,
  ipc_renderHighlights,
  ipc_replaceProjectPlusCard,
  ipc_setChatKey,
  ipc_softResetDolphin,
  ipc_storePlayKeyFile,
  ipc_viewSlpReplay,
} from "./ipc";
import type { DolphinManager } from "./manager";
import { findMissingDlls } from "./missing_dlls";
import { deletePlayKeyFile, writePlayKeyFile } from "./playkey";
import type { RenderJob } from "./render_queue";
import { clearRoomRequest, handOffRoom, isHandOffCode, writeRoomRequest } from "./room_handoff";
import { installRosettaElevated } from "./rosetta/install_rosetta";
import { DolphinEventType, DolphinLaunchType } from "./types";

/** A create request's extras for the log: " (ruleset BCDFGH, private)". */
const roomDetails = (ruleset?: string, isPrivate?: boolean) => {
  const parts = [ruleset ? `ruleset ${ruleset}` : "", isPrivate ? "private" : ""].filter(Boolean);
  return parts.length > 0 ? ` (${parts.join(", ")})` : "";
};

export default function setupDolphinIpc({ dolphinManager }: { dolphinManager: DolphinManager }) {
  dolphinManager.events.subscribe((event) => {
    void ipc_dolphinEvent.main!.trigger(event).catch(log.error);
  });

  ipc_downloadDolphin.main!.handle(async ({ dolphinType }) => {
    await dolphinManager.installDolphin(dolphinType);
    return { success: true };
  });

  ipc_configureDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("configuring dolphin...");
    await dolphinManager.configureDolphin(dolphinType);
    return { success: true };
  });

  ipc_softResetDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("soft resetting dolphin...");
    await dolphinManager.reinstallDolphin(dolphinType, false);
    return { success: true };
  });

  ipc_openDolphinSettingsFolder.main!.handle(async ({ dolphinType }) => {
    // Our Dolphin always runs with `-u <User folder>`, so that is where its settings are.
    const dolphinInstall = dolphinManager.getInstallation(dolphinType);
    await dolphinInstall.ensureUserFolder();
    await shell.openPath(dolphinInstall.userFolder);
    return { success: true };
  });

  ipc_hardResetDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("hard resetting dolphin...");
    await dolphinManager.reinstallDolphin(dolphinType, true);
    return { success: true };
  });

  ipc_getDolphinPaths.main!.handle(async ({ dolphinType }) => {
    const installation = dolphinManager.getInstallation(dolphinType);
    const userFolder = installation.userFolder;
    const playKeyFile = path.join(userFolder, DOLPHIN_ONLINE_DIR, "user.json");
    return {
      executable: dolphinManager.getDolphinExecutablePath(dolphinType),
      userFolder,
      playKeyFile,
      gameFilesFolder: dolphinManager.netplayFolder(),
    };
  });

  ipc_moveGameFiles.main!.handle(async ({ folder }) => {
    try {
      const result = await dolphinManager.moveGameFiles(folder);
      return result.action === "cancelled" ? { action: "cancelled", folder: result.folder } : result;
    } catch (err) {
      log.error(err);
      return { action: "error", message: err instanceof Error ? err.message : String(err) };
    }
  });

  ipc_storePlayKeyFile.main!.handle(async ({ key }) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await writePlayKeyFile(installation, key);
    return { success: true };
  });

  ipc_checkPlayKeyExists.main!.handle(async ({ key }) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    const keyPath = await installation.findPlayKey();
    const exists = await fileExists(keyPath);
    if (!exists) {
      return { exists: false };
    }

    try {
      const jsonKey = await readFile(keyPath);
      const fileContents = jsonKey.toString();
      const storedKey = JSON.parse(fileContents);
      return { exists: isEqual(storedKey, key) };
    } catch (err) {
      log.warn(err);
      return { exists: false };
    }
  });

  ipc_removePlayKeyFile.main!.handle(async () => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await deletePlayKeyFile(installation);
    return { success: true };
  });

  // Replays play on the Project+ release they were recorded on (docs/design/pplus-versions.md):
  // when that release's files are not here yet, the renderer is told so (and asks) instead.
  ipc_viewSlpReplay.main!.handle(async ({ files, choice = "auto", commandId, nav }) => {
    const d = await dolphinManager.decideReplayRelease(
      files.map((f) => f.path),
      choice,
    );
    if (d.result.status === "playing") {
      await dolphinManager.launchPlaybackDolphin(
        "playback",
        {
          mode: "queue",
          queue: files.filter((f) => d.paths.includes(f.path)),
          playAnyway: d.playAnyway || undefined,
          commandId,
          nav,
        },
        { pplusRelease: d.release },
      );
    }
    return d.result;
  });

  ipc_playHighlights.main!.handle(async ({ items, commandId, choice = "auto", list, updateOnly }) => {
    // (An update of the list's stars and count: only to a Dolphin playing it, on its release already.)
    if (updateOnly && !dolphinManager.isPlaybackOpen("playback")) {
      return { status: "playing" as const, release: null, skipped: 0 };
    }
    const d = await dolphinManager.decideReplayRelease(
      items.map((i) => i.path),
      choice,
    );
    if (d.result.status === "playing") {
      await dolphinManager.launchPlaybackDolphin(
        "playback",
        {
          mode: "highlights",
          queue: items.filter((i) => d.paths.includes(i.path)),
          commandId,
          playAnyway: d.playAnyway || undefined,
          marks: list?.marks,
          nav: list?.nav,
        },
        { pplusRelease: d.release },
      );
    }
    return d.result;
  });

  // Rendering (render_queue.ts): every replay on its own Project+ release, so a request whose
  // releases are not all here yet is answered as a play request is, and the renderer asks.
  ipc_renderHighlights.main!.handle(async ({ request, choice = "auto" }) => {
    const { names, music, combo, wholeMatch } = request;
    // (A whole match: its frames from the file.)
    const items = wholeMatch ? await Promise.all(request.items.map((i) => matchFrames(i.path))) : request.items;
    if (!items.length) {
      return { status: "queued" as const, jobs: [] };
    }
    const d = await dolphinManager.decideRenderReleases(
      items.map((i) => i.path),
      choice,
    );
    if (d.result) {
      return d.result;
    }
    const resolution = dolphinManager.clipResolution();
    const job = (name: string, pieces: RenderJob["pieces"]): RenderJob => ({
      name,
      music,
      playAnyway: d.playAnyway,
      resolution,
      pieces,
      wholeMatch,
    });
    let jobs: RenderJob[];
    if (combo) {
      // One movie, in the list's order: a run of clips recorded on another Project+ release than
      // the one before is another piece (a Dolphin plays one release's card), the pieces joined.
      const pieces: RenderJob["pieces"] = [];
      items.forEach((item, i) => {
        const last = pieces[pieces.length - 1];
        if (last && last.release === d.releases[i]) {
          last.items.push(item);
        } else {
          pieces.push({ release: d.releases[i], items: [item] });
        }
      });
      jobs = [job(names[0], pieces)];
    } else {
      jobs = items.map((item, i) => job(names[i] ?? names[0], [{ release: d.releases[i], items: [item] }]));
    }
    return { status: "queued" as const, jobs: dolphinManager.renders.add(jobs) };
  });

  ipc_cancelRender.main!.handle(async ({ jobId }) => {
    await dolphinManager.renders.cancel(jobId);
    return { success: true };
  });

  ipc_getRenderJobs.main!.handle(async () => {
    return { jobs: dolphinManager.renders.jobs() };
  });

  ipc_findMissingDlls.main!.handle(async ({ dolphinType }) => {
    const exe = await dolphinManager.getInstallation(dolphinType).findDolphinExecutable();
    const dlls = await findMissingDlls(exe);
    log.warn(
      `Dolphin could not load: ${
        dlls.map((d) => `${d.name} (${d.problem}, imported by ${d.importedBy})`).join(", ") || "no missing DLL found"
      }`,
    );
    return { dlls };
  });

  ipc_getPPlusVersions.main!.handle(async () => {
    return await dolphinManager.pplusVersionStates();
  });

  ipc_preparePPlusRelease.main!.handle(async ({ release, importFrom }) => {
    try {
      const state = await dolphinManager.preparePPlusRelease(release, {
        importFrom,
        onProgress: ({ phase, current, total }) => {
          void ipc_dolphinEvent
            .main!.trigger({ type: DolphinEventType.PPLUS_PREPARE_PROGRESS, release, phase, current, total })
            .catch(log.error);
        },
      });
      return { ok: true as const, state };
    } catch (err) {
      log.error(err);
      const code = err instanceof PPlusVersionError ? err.code : "error";
      return { ok: false as const, code, message: err instanceof Error ? err.message : String(err) };
    }
  });

  ipc_removePPlusRelease.main!.handle(async ({ release }) => {
    await dolphinManager.removePPlusRelease(release);
    return { success: true };
  });

  ipc_launchNetplayDolphin.main!.handle(async ({ bootGame }) => {
    return await dolphinManager.launchNetplayDolphin({ bootGame });
  });

  ipc_replaceProjectPlusCard.main!.handle(async () => {
    await dolphinManager.replaceProjectPlusCard();
    return { success: true };
  });

  ipc_closeNetplayIfIdle.main!.handle(async () => {
    return { closed: await dolphinManager.closeNetplayIfIdle() };
  });

  ipc_joinRoomInGame.main!.handle(async ({ code, ruleset, isPrivate }) => {
    if (!isHandOffCode(code)) {
      throw new Error(`Not a room or connect code: ${code}`);
    }
    if (!dolphinManager.isNetplayRunning()) {
      return { outcome: "not-running" };
    }
    // A Dolphin open without a game (its game was stopped, or Configure's) closes, and the click
    // starts one. Dolphin itself says (quit_if_idle.ts): game-status.json stays fresh after a
    // stopped game, so a hand-off to it was "accepted" and never happened.
    if (await dolphinManager.closeNetplayIfIdle()) {
      return { outcome: "not-running" };
    }
    const onlineDir = dolphinManager.netplayOnlineDir();
    const result = await handOffRoom(onlineDir, code, { ruleset, isPrivate });
    log.info(`Room ${code}${roomDetails(ruleset, isPrivate)} handed to the running game: ${result.outcome}`);
    return result;
  });

  ipc_prepareRoomLaunch.main!.handle(async ({ code, ruleset, isPrivate }) => {
    const onlineDir = dolphinManager.netplayOnlineDir();
    await mkdir(onlineDir, { recursive: true });
    const req = await writeRoomRequest(onlineDir, code, new Date(), ruleset, isPrivate);
    log.info(`Room ${code}${roomDetails(ruleset, isPrivate)} requested for the game about to start (${req.id})`);
    return { id: req.id };
  });

  ipc_cancelRoomLaunch.main!.handle(async ({ id }) => {
    await clearRoomRequest(dolphinManager.netplayOnlineDir(), id);
    return { success: true };
  });

  ipc_getChatKey.main!.handle(async () => {
    const chatKey = await dolphinManager.getInstallation(DolphinLaunchType.NETPLAY).getChatKey();
    return { chatKey };
  });

  ipc_setChatKey.main!.handle(async ({ press }) => {
    const chatKey = chatKeyExpression(press, process.platform);
    if (chatKey !== null) {
      await dolphinManager.getInstallation(DolphinLaunchType.NETPLAY).setChatKey(chatKey);
    }
    return { chatKey };
  });

  ipc_installRosetta.main!.handle(async () => {
    const exitCode = await installRosettaElevated();
    return { exitCode };
  });

  return { dolphinManager };
}
