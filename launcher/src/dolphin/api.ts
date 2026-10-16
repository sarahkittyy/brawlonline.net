/* eslint-disable import/no-default-export */

import type { ChatKeyPress } from "@common/chat_key";

import {
  ipc_cancelRoomLaunch,
  ipc_checkPlayKeyExists,
  ipc_configureDolphin,
  ipc_dolphinEvent,
  ipc_downloadDolphin,
  ipc_getChatKey,
  ipc_getDolphinPaths,
  ipc_hardResetDolphin,
  ipc_installRosetta,
  ipc_joinRoomInGame,
  ipc_launchNetplayDolphin,
  ipc_moveGameFiles,
  ipc_openDolphinSettingsFolder,
  ipc_prepareRoomLaunch,
  ipc_removePlayKeyFile,
  ipc_setChatKey,
  ipc_softResetDolphin,
  ipc_storePlayKeyFile,
  ipc_viewSlpReplay,
} from "./ipc";
import type {
  DolphinEventMap,
  DolphinEventType,
  DolphinLaunchType,
  DolphinService,
  PlayKey,
  ReplayQueueItem,
} from "./types";

const dolphinApi: DolphinService = {
  async installRosetta(): Promise<{ exitCode: number }> {
    const { result } = await ipc_installRosetta.renderer!.trigger({});
    return { exitCode: result.exitCode };
  },
  async getDolphinPaths(dolphinType: DolphinLaunchType) {
    const { result } = await ipc_getDolphinPaths.renderer!.trigger({ dolphinType });
    return result;
  },
  async moveGameFiles(folder: string | null) {
    const { result } = await ipc_moveGameFiles.renderer!.trigger({ folder });
    return result;
  },
  async downloadDolphin(dolphinType: DolphinLaunchType) {
    await ipc_downloadDolphin.renderer!.trigger({ dolphinType });
  },
  async configureDolphin(dolphinType: DolphinLaunchType) {
    await ipc_configureDolphin.renderer!.trigger({ dolphinType });
  },
  async softResetDolphin(dolphinType: DolphinLaunchType) {
    await ipc_softResetDolphin.renderer!.trigger({ dolphinType });
  },
  async hardResetDolphin(dolphinType: DolphinLaunchType) {
    await ipc_hardResetDolphin.renderer!.trigger({ dolphinType });
  },
  async openDolphinSettingsFolder(dolphinType: DolphinLaunchType) {
    await ipc_openDolphinSettingsFolder.renderer!.trigger({ dolphinType });
  },
  async storePlayKeyFile(key: PlayKey) {
    await ipc_storePlayKeyFile.renderer!.trigger({ key });
  },
  async checkPlayKeyExists(key: PlayKey): Promise<boolean> {
    const { result } = await ipc_checkPlayKeyExists.renderer!.trigger({ key });
    return result.exists;
  },
  async removePlayKeyFile(): Promise<void> {
    await ipc_removePlayKeyFile.renderer!.trigger({});
  },
  async viewSlpReplay(files: ReplayQueueItem[]): Promise<void> {
    await ipc_viewSlpReplay.renderer!.trigger({ files });
  },
  async launchNetplayDolphin(opts?: { bootGame?: boolean }): Promise<void> {
    await ipc_launchNetplayDolphin.renderer!.trigger({ bootGame: opts?.bootGame });
  },
  async joinRoomInGame(code: string, ruleset?: string) {
    const { result } = await ipc_joinRoomInGame.renderer!.trigger({ code, ruleset });
    return result;
  },
  async prepareRoomLaunch(code: string, ruleset?: string) {
    const { result } = await ipc_prepareRoomLaunch.renderer!.trigger({ code, ruleset });
    return result;
  },
  async cancelRoomLaunch(id: string) {
    await ipc_cancelRoomLaunch.renderer!.trigger({ id });
  },
  async getChatKey(): Promise<string> {
    const { result } = await ipc_getChatKey.renderer!.trigger({});
    return result.chatKey;
  },
  async setChatKey(press: ChatKeyPress): Promise<string | null> {
    const { result } = await ipc_setChatKey.renderer!.trigger({ press });
    return result.chatKey;
  },
  onEvent<T extends DolphinEventType>(eventType: T, handle: (event: DolphinEventMap[T]) => void) {
    const { destroy } = ipc_dolphinEvent.renderer!.handle(async (result) => {
      if (result.type === eventType) {
        handle(result as DolphinEventMap[T]);
      }
    });
    return destroy;
  },
};

export default dolphinApi;
