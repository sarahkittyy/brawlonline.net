import type { AccountsManager } from "@accounts/accounts_manager";
import { appVersion } from "@common/constants";
import { PRODUCT_SLUG } from "@common/product";
import type { DolphinManager } from "@dolphin/manager";
import type { SettingsManager } from "@settings/settings_manager";
import log from "electron-log";
import { rm } from "fs/promises";

import {
  ipc_spectateGetState,
  ipc_spectateList,
  ipc_spectateSetAutoPlay,
  ipc_spectateStateChangedEvent,
  ipc_spectateStop,
  ipc_spectateWatch,
} from "./ipc";
import { SpectateClient } from "./spectate_client";

const PLAYBACK = "playback";

export default function setupSpectateIpc({
  accountsManager,
  accountsApi,
  dolphinManager,
  settingsManager,
}: {
  accountsManager: AccountsManager;
  accountsApi: string;
  dolphinManager: DolphinManager;
  settingsManager: SettingsManager;
}) {
  // Spectated games are a cache, not replays: cleared at every start (docs/design/spectating.md §6.1).
  void rm(dolphinManager.spectateDir(), { recursive: true, force: true }).catch((err) =>
    log.warn(`spectate: could not clear the old games: ${err}`),
  );

  const client = new SpectateClient({
    baseUrl: accountsApi.replace(/\/+$/, ""),
    userAgent: `${PRODUCT_SLUG}-launcher/${appVersion}`,
    getToken: (uid) => accountsManager.sessionToken(uid),
    spectateDir: () => dolphinManager.spectateDir(),
    play: (comm) => dolphinManager.launchPlaybackDolphin(PLAYBACK, comm),
    playbackStatus: () => dolphinManager.playbackStatus(PLAYBACK),
    isPlaybackOpen: () => dolphinManager.isPlaybackOpen(PLAYBACK),
    closePlayback: async () => dolphinManager.closePlaybackDolphin(PLAYBACK),
    closeOnEnd: () => settingsManager.getCloseDolphinAfterSpectating(),
  });
  client.onState((state) => void ipc_spectateStateChangedEvent.main!.trigger({ state }));

  ipc_spectateList.main!.handle(async ({ uid }) => ({ list: await client.list(uid) }));
  ipc_spectateWatch.main!.handle(async ({ uid, stream }) => {
    try {
      return { result: await client.watch(uid, stream) };
    } catch (err) {
      log.error(err);
      return { result: { ok: false as const, message: err instanceof Error ? err.message : String(err) } };
    }
  });
  ipc_spectateStop.main!.handle(async () => {
    client.stop();
    return { success: true };
  });
  ipc_spectateGetState.main!.handle(async () => ({ state: client.getState() }));
  ipc_spectateSetAutoPlay.main!.handle(async ({ on }) => {
    client.setAutoPlay(on);
    return { success: true };
  });
  return client;
}
