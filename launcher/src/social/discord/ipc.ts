import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { DiscordActivity, DiscordGameState } from "./types";

/** Shows the activity on the player's Discord profile (null: none). */
export const ipc_discordSetActivity = makeEndpoint.main(
  "discord_setActivity",
  <{ activity: DiscordActivity | null }>_,
  <SuccessPayload>_,
);

/** What the Dolphins the launcher started show now. */
export const ipc_discordGameState = makeEndpoint.main(
  "discord_gameState",
  <EmptyPayload>_,
  <{ state: DiscordGameState }>_,
);
