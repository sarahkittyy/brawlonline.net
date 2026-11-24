/* eslint-disable import/no-default-export */
import { ipc_discordGameState, ipc_discordSetActivity } from "./ipc";
import type { DiscordActivity, DiscordGameState } from "./types";

const discordApi = {
  async setActivity(activity: DiscordActivity | null): Promise<void> {
    await ipc_discordSetActivity.renderer!.trigger({ activity });
  },
  async gameState(): Promise<DiscordGameState> {
    const { result } = await ipc_discordGameState.renderer!.trigger({});
    return result.state;
  },
};

export default discordApi;
