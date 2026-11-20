// EmissaryRecomp (emissary.ts): Settings > Add-ons' card, and the auto-update.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { readTestMode } from "@common/test_mode";
import { IsoValidity } from "@common/types";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import electronLog from "electron-log";
import path from "path";

import type { DolphinManager } from "../manager";
import { EMISSARY_FOLDER, EmissaryCancelled, EmissaryRecomp, localReleasesFetch } from "./emissary";
import {
  ipc_emissaryCancel,
  ipc_emissaryGetState,
  ipc_emissaryInstall,
  ipc_emissaryRemove,
  ipc_emissaryState,
} from "./ipc";

const log = electronLog.scope("dolphin/emissary");

const AUTO_UPDATE_FIRST_MS = 60 * 1000;
/** How often the auto-update looks for a newer release while Play plays with EmissaryRecomp... */
const AUTO_UPDATE_PLAYING_MS = 30 * 60 * 1000;
/** ...and otherwise (installed, Dolphin chosen). */
const AUTO_UPDATE_EVERY_MS = 6 * 60 * 60 * 1000;

// eslint-disable-next-line import/no-default-export
export default function setupEmissaryIpc({
  dolphinManager,
  settingsManager,
  checkIso,
}: {
  dolphinManager: DolphinManager;
  settingsManager: SettingsManager;
  checkIso: (isoPath: string) => Promise<IsoValidity>;
}): () => EmissaryRecomp {
  // The netplay folder can move (Settings > Dolphin): one store per folder.
  let store: EmissaryRecomp | null = null;
  // The Settings disc's check (cached on disk by checkIso); the add-on's state reads it synchronously.
  let discValid = false;
  const isoPath = () => settingsManager.get().settings.isoPath;
  const recheckDisc = async () => {
    const iso = isoPath();
    discValid = iso ? (await checkIso(iso).catch(() => IsoValidity.UNVALIDATED)) === IsoValidity.VALID : false;
    if (store) {
      await ipc_emissaryState.main!.trigger(await store.state()).catch(log.warn);
    }
  };
  settingsManager.onSettingChange("isoPath", () => {
    discValid = false;
    void recheckDisc();
  });
  void recheckDisc();

  const emissary = (): EmissaryRecomp => {
    const root = path.join(dolphinManager.netplayFolder(), EMISSARY_FOLDER);
    if (!store || store.root !== root) {
      const test = readTestMode(process.env, app.isPackaged);
      const testing = test.active || !app.isPackaged;
      // Releases from a folder instead of GitHub (`PPO_EMISSARY_RELEASES=<dir>`, `<dir>/<tag>/`
      // with the release's assets), to try installs and updates with local builds.
      const localReleases = testing ? process.env.PPO_EMISSARY_RELEASES?.trim() || undefined : undefined;
      if (localReleases) {
        log.warn(`EmissaryRecomp releases come from ${localReleases} (PPO_EMISSARY_RELEASES)`);
      }
      store = new EmissaryRecomp({
        root,
        isoPath,
        isoValid: () => discValid,
        dolphinTool: () => dolphinManager.dolphinToolPath(),
        // Installs an older release, to try updating (`PPO_EMISSARY_TAG=v0.6.0`).
        tag: testing ? process.env.PPO_EMISSARY_TAG?.trim() || undefined : undefined,
        ...(localReleases ? { fetch: localReleasesFetch(localReleases) } : {}),
        log: (message) => log.info(message),
        onChange: (state) => {
          void ipc_emissaryState.main!.trigger(state).catch(log.error);
        },
      });
    }
    return store;
  };

  ipc_emissaryGetState.main!.handle(async ({ refresh }) => {
    return await emissary().state({ refresh });
  });

  ipc_emissaryInstall.main!.handle(async () => {
    try {
      return { ok: true as const, state: await emissary().install() };
    } catch (err) {
      return {
        ok: false as const,
        cancelled: err instanceof EmissaryCancelled,
        message: err instanceof Error ? err.message : String(err),
      };
    }
  });

  ipc_emissaryCancel.main!.handle(async () => {
    emissary().cancel();
    return { success: true };
  });

  ipc_emissaryRemove.main!.handle(async () => {
    await emissary().remove();
    // Nothing left to play with: Play uses Dolphin again.
    if (settingsManager.get().settings.playProjectPlusWith === "emissary") {
      await settingsManager.updateSetting("playProjectPlusWith", "dolphin");
    }
    return { success: true };
  });

  // "Keep up to date automatically": a newer release downloads by itself (staged while the game is
  // open, emissary.ts; a room, Direct or queue click then starts it once the game is idle,
  // DolphinManager.closeEmissaryForUpdate). Only once installed; never the first download. Every
  // 30 minutes while Play plays with it, else every 6 hours. Play itself downloads a newer release
  // before it starts the game whatever this says (prepareForPlay).
  let lastAutoUpdate = 0;
  const autoUpdate = async () => {
    const { emissaryAutoUpdate, playProjectPlusWith } = settingsManager.get().settings;
    if (!emissaryAutoUpdate) {
      return;
    }
    const every = playProjectPlusWith === "emissary" ? AUTO_UPDATE_PLAYING_MS : AUTO_UPDATE_EVERY_MS;
    if (Date.now() - lastAutoUpdate < every - 1000) {
      return;
    }
    lastAutoUpdate = Date.now();
    const em = emissary();
    const state = await em.state({ refresh: true });
    if (state.installed && !state.upToDate && !state.job) {
      log.info(`EmissaryRecomp: updating ${state.installed.version} to ${state.latest?.version}`);
      await em
        .install()
        .catch((err) => log.warn(`EmissaryRecomp auto-update: ${err instanceof Error ? err.message : err}`));
    }
  };
  setTimeout(() => {
    void autoUpdate();
    setInterval(() => void autoUpdate(), AUTO_UPDATE_PLAYING_MS).unref();
  }, AUTO_UPDATE_FIRST_MS).unref();

  return emissary;
}
