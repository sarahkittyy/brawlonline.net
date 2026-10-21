/* eslint-disable import/no-default-export */

import type { ChatKeyPress } from "@common/chat_key";
import type { HighlightPlayItem } from "@replays/highlights/types";

import {
  ipc_cancelRender,
  ipc_cancelRoomLaunch,
  ipc_checkPlayKeyExists,
  ipc_closeNetplayIfIdle,
  ipc_configureDolphin,
  ipc_dolphinEvent,
  ipc_downloadDolphin,
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
import type {
  DolphinEventMap,
  DolphinEventType,
  DolphinLaunchType,
  DolphinService,
  PlayKey,
  RenderRequest,
  ReplayPlayChoice,
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
  async viewSlpReplay(files: ReplayQueueItem[], choice?: ReplayPlayChoice) {
    const { result } = await ipc_viewSlpReplay.renderer!.trigger({ files, choice });
    return result;
  },
  async playHighlights(items: HighlightPlayItem[], commandId: string, choice?: ReplayPlayChoice) {
    const { result } = await ipc_playHighlights.renderer!.trigger({ items, commandId, choice });
    return result;
  },
  async renderHighlights(request: RenderRequest, choice?: ReplayPlayChoice) {
    const { result } = await ipc_renderHighlights.renderer!.trigger({
      request,
      choice,
    });
    return result;
  },
  async cancelRender(jobId: string) {
    await ipc_cancelRender.renderer!.trigger({ jobId });
  },
  async getRenderJobs() {
    const { result } = await ipc_getRenderJobs.renderer!.trigger({});
    return result.jobs;
  },
  async getPPlusVersions() {
    const { result } = await ipc_getPPlusVersions.renderer!.trigger({});
    return result;
  },
  async preparePPlusRelease(release: string, importFrom?: string) {
    const { result } = await ipc_preparePPlusRelease.renderer!.trigger({ release, importFrom });
    return result;
  },
  async removePPlusRelease(release: string) {
    await ipc_removePPlusRelease.renderer!.trigger({ release });
  },
  async launchNetplayDolphin(opts?: { bootGame?: boolean }) {
    const { result } = await ipc_launchNetplayDolphin.renderer!.trigger({ bootGame: opts?.bootGame });
    return result;
  },
  async replaceProjectPlusCard() {
    await ipc_replaceProjectPlusCard.renderer!.trigger({});
  },
  async closeNetplayIfIdle() {
    const { result } = await ipc_closeNetplayIfIdle.renderer!.trigger({});
    return result.closed;
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
