import type { DolphinManager } from "@dolphin/manager";
import { readGameStatus } from "@dolphin/room_handoff";
import { readReplayHeader } from "@replays/replay_format";
import { app } from "electron";
import electronLog from "electron-log";

import { DiscordRpc } from "./discord_rpc";
import { ipc_discordGameState, ipc_discordSetActivity } from "./ipc";
import type { DiscordGame, DiscordGameState, DiscordReplay } from "./types";
import { DISCORD_APPLICATION_ID } from "./types";

const log = electronLog.scope("discord");

type RawPlayer = {
  port?: number;
  uid?: string;
  displayName?: string;
  connectCode?: string;
  characterId?: number;
  teamId?: number | null;
};

type RawGame = {
  mode?: string;
  gameIndex?: number;
  stageId?: number;
  isTeams?: boolean;
  startTime?: string;
  players?: RawPlayer[];
  recorder?: { port?: number };
};

/** game-status.json's `game` (the replay's INFO JSON Dolphin starts the game with), checked. */
export function readStatusGame(raw: unknown): DiscordGame | null {
  if (!raw || typeof raw !== "object") {
    return null;
  }
  const g = raw as RawGame;
  if (!Array.isArray(g.players)) {
    return null;
  }
  const num = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);
  return {
    mode: typeof g.mode === "string" ? g.mode : "unknown",
    gameIndex: num(g.gameIndex) ?? 0,
    stageId: num(g.stageId),
    isTeams: g.isTeams === true,
    startTime: typeof g.startTime === "string" ? g.startTime : null,
    players: g.players.map((p) => ({
      port: num(p.port) ?? 0,
      uid: typeof p.uid === "string" ? p.uid : "",
      displayName: typeof p.displayName === "string" ? p.displayName : "",
      connectCode: typeof p.connectCode === "string" ? p.connectCode : "",
      characterId: num(p.characterId) ?? -1,
      teamId: num(p.teamId),
    })),
    ownPort: num(g.recorder?.port),
  };
}

export default function setupDiscordIpc({ dolphinManager }: { dolphinManager: DolphinManager }) {
  const rpc = new DiscordRpc(DISCORD_APPLICATION_ID);
  app.on("will-quit", () => rpc.close());

  ipc_discordSetActivity.main!.handle(async ({ activity }) => {
    rpc.setActivity(activity);
    return { success: true as const };
  });

  // The replay the playback Dolphin plays: its header, read once per file.
  let replayPath: string | null = null;
  let replay: DiscordReplay | null = null;
  const replayOf = async (file: string | undefined): Promise<DiscordReplay | null> => {
    if (!file) {
      return null;
    }
    if (file !== replayPath) {
      replayPath = file;
      replay = null;
      try {
        const { meta } = await readReplayHeader(file);
        replay = {
          stageId: meta.stageId,
          players: meta.players.map((p) => ({
            displayName: p.displayName,
            characterId: p.characterId,
            teamId: p.teamId,
          })),
        };
      } catch (err) {
        log.info(`No replay header for the activity: ${err instanceof Error ? err.message : String(err)}`);
      }
    }
    return replay;
  };

  ipc_discordGameState.main!.handle(async () => {
    const netplayOpen = dolphinManager.isNetplayRunning();
    const otherGame = dolphinManager.runningOtherGame();
    let screen: string | null = null;
    let scene: string | null = null;
    let game: DiscordGame | null = null;
    if (netplayOpen && !otherGame) {
      const read = await readGameStatus(dolphinManager.netplayOnlineDir());
      if (read.kind === "fresh") {
        const s = read.status as typeof read.status & { scene?: unknown; game?: unknown };
        screen = s.screen ?? null;
        scene = typeof s.scene === "string" && s.scene ? s.scene : null;
        game = readStatusGame(s.game);
      }
    }
    const playbackOpen = dolphinManager.isPlaybackOpen();
    const status = playbackOpen ? await dolphinManager.playbackStatus() : null;
    const state: DiscordGameState = {
      netplayOpen,
      otherGame,
      screen,
      scene,
      game,
      playbackOpen,
      replay: status?.state === "playing" ? await replayOf(status.replay) : null,
    };
    return { state };
  });
}
