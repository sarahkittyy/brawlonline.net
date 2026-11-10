// Project Punch (project_punch.ts): Home's lab button, its dialog and Settings > Training.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { readTestMode } from "@common/test_mode";
import { app } from "electron";
import electronLog from "electron-log";
import path from "path";

import { otherProjectPlusBuild } from "../install/pplus_builds";
import type { DolphinManager } from "../manager";
import {
  ipc_projectPunchCancel,
  ipc_projectPunchGetState,
  ipc_projectPunchInstall,
  ipc_projectPunchPlay,
  ipc_projectPunchRemove,
  ipc_projectPunchState,
} from "./ipc";
import { PROJECT_PUNCH_FOLDER, ProjectPunch, ProjectPunchCancelled } from "./project_punch";

const log = electronLog.scope("dolphin/project_punch");

// eslint-disable-next-line import/no-default-export
export default function setupProjectPunchIpc({ dolphinManager }: { dolphinManager: DolphinManager }) {
  // The netplay folder can move (Settings > Dolphin): one store per folder.
  let store: ProjectPunch | null = null;
  const projectPunch = (): ProjectPunch => {
    const root = path.join(dolphinManager.netplayFolder(), PROJECT_PUNCH_FOLDER);
    if (!store || store.root !== root) {
      const test = readTestMode(process.env, app.isPackaged);
      store = new ProjectPunch({
        root,
        sourceCard: () => dolphinManager.plainSdCardImage(),
        pplusRelease: () => dolphinManager.pplusReleaseVersion(),
        otherPPlusBuild: otherProjectPlusBuild,
        // Installs an older release, to try updating (`PPO_PROJECT_PUNCH_TAG=v1.3.0`).
        tag: test.active || !app.isPackaged ? process.env.PPO_PROJECT_PUNCH_TAG?.trim() || undefined : undefined,
        log: (message) => log.info(message),
        onChange: (state) => {
          void ipc_projectPunchState.main!.trigger(state).catch(log.error);
        },
      });
    }
    return store;
  };

  ipc_projectPunchGetState.main!.handle(async ({ refresh }) => {
    return await projectPunch().state({ refresh });
  });

  ipc_projectPunchInstall.main!.handle(async () => {
    try {
      return { ok: true as const, state: await projectPunch().install() };
    } catch (err) {
      return {
        ok: false as const,
        cancelled: err instanceof ProjectPunchCancelled,
        message: err instanceof Error ? err.message : String(err),
      };
    }
  });

  ipc_projectPunchCancel.main!.handle(async () => {
    projectPunch().cancel();
    return { success: true };
  });

  ipc_projectPunchRemove.main!.handle(async () => {
    await projectPunch().remove();
    return { success: true };
  });

  ipc_projectPunchPlay.main!.handle(async () => {
    const pp = projectPunch();
    const card = await pp.cardForPlay();
    pp.setRunning(true);
    await dolphinManager.launchProjectPunchDolphin(card, () => pp.setRunning(false));
    return { success: true };
  });
}
