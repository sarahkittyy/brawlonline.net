import type { DiscordGame, DiscordGameState } from "@social/discord/types";
import type { QueueState, SocialPlayer, SocialRoom } from "@social/types";
import type { SpectateState } from "@spectate/types";
import { describe, expect, it } from "vitest";

import type { ActivityInput } from "./discord_activity";
import { buildDiscordActivity, LOGO_IMAGE, rankImage } from "./discord_activity";

const player = (uid: string, displayName: string): SocialPlayer => ({ uid, displayName, connectCode: "ABCD#1" });
const PEACHY = player("p", "Peachy");

const gameState = (over: Partial<DiscordGameState> = {}): DiscordGameState => ({
  netplayOpen: true,
  otherGame: null,
  screen: "menus",
  scene: null,
  game: null,
  playbackOpen: false,
  replay: null,
  ...over,
});

const playing = (over: Partial<DiscordGame> = {}): DiscordGame => ({
  mode: "ranked",
  gameIndex: 2,
  stageId: 1,
  isTeams: false,
  startTime: "2026-11-24T10:00:00.000Z",
  players: [
    { port: 1, uid: "me", displayName: "sarahkitty", connectCode: "SARA#694", characterId: 0x14, teamId: null },
    { port: 2, uid: "p", displayName: "Peachy", connectCode: "PEAC#33", characterId: 0x0d, teamId: null },
  ],
  ownPort: 1,
  ...over,
});

const input = (over: Partial<ActivityInput> = {}): ActivityInput => ({
  launcherStartedAt: 1000,
  phaseStartedAt: 2000,
  you: { uid: "me", displayName: "sarahkitty" },
  game: null,
  queue: null,
  partner: null,
  socialRoom: null,
  room: null,
  spectate: null,
  rank: { tier: "gold", division: 1 },
  showPlayers: true,
  buttons: false,
  ...over,
});

const rankedSet = (over: Partial<Extract<QueueState, { state: "playing" }>> = {}): QueueState => ({
  state: "playing",
  mode: "ranked",
  opponents: [PEACHY],
  game: 1,
  wins: [0, 0],
  ...over,
});

describe("buildDiscordActivity", () => {
  it("says In the launcher with no game open", () => {
    expect(buildDiscordActivity(input())).toEqual({
      largeImage: LOGO_IMAGE,
      details: "In the launcher",
      startedAt: 1000,
    });
  });

  it("shows a Ranked search with the rank badge", () => {
    const a = buildDiscordActivity(
      input({
        game: gameState({ screen: "searching" }),
        queue: { state: "searching", mode: "ranked", since: "2026-11-24T10:00:00.000Z" },
      }),
    );
    expect(a.details).toBe("Ranked");
    expect(a.state).toBe("Searching");
    expect(a.smallImage).toBe("https://brawlonline.net/assets/ranks/gold-1.png");
    expect(a.smallText).toBe("Gold 1");
    expect(a.startedAt).toBe(Date.parse("2026-11-24T10:00:00.000Z"));
  });

  it("follows a Ranked set: character select, a game, the stage select between games", () => {
    const css = buildDiscordActivity(
      input({ game: gameState({ screen: "online-css", scene: "scSelCharacter" }), queue: rankedSet() }),
    );
    expect([css.details, css.state]).toEqual(["Ranked · vs Peachy", "Game 1 · Character Select"]);
    // Between games: the logo and the rank badge.
    expect([css.largeImage, css.smallImage]).toEqual([LOGO_IMAGE, "https://brawlonline.net/assets/ranks/gold-1.png"]);

    const inGame = buildDiscordActivity(
      input({
        game: gameState({ screen: "match", scene: "scMelee", game: playing() }),
        queue: rankedSet({ game: 2, wins: [1, 0], liveId: "0123456789ab" }),
        buttons: true,
      }),
    );
    expect([inGame.details, inGame.state, inGame.largeText]).toEqual([
      "Ranked · vs Peachy",
      "Game 2 · 1 - 0",
      "Battlefield",
    ]);
    expect(inGame.startedAt).toBe(Date.parse("2026-11-24T10:00:00.000Z"));
    // In a game: the stage and the player's character (Discord art assets).
    expect([inGame.largeImage, inGame.smallImage, inGame.smallText]).toEqual([
      "stage_1",
      "char_mr_game_and_watch",
      "Mr. Game & Watch",
    ]);
    expect(inGame.buttons).toEqual([{ label: "Watch their game", url: "https://brawlonline.net/?watch=0123456789ab" }]);

    const sss = buildDiscordActivity(
      input({ game: gameState({ screen: "match", scene: "scSelStage" }), queue: rankedSet({ game: 3, wins: [1, 1] }) }),
    );
    expect(sss.state).toBe("Game 3 · 1 - 1 · Stage Select");
    expect(sss.startedAt).toBe(2000);
  });

  it("leaves the names out when Show who I am playing is off", () => {
    const a = buildDiscordActivity(
      input({ game: gameState({ screen: "match", game: playing() }), queue: rankedSet(), showPlayers: false }),
    );
    expect(a.details).toBe("Ranked");
    expect(a.buttons).toBeUndefined();
  });

  it("names a 2v2's opponents and partner", () => {
    const twos = playing({
      mode: "twos",
      isTeams: true,
      players: [
        { port: 1, uid: "me", displayName: "sarahkitty", connectCode: "", characterId: 0x14, teamId: 0 },
        { port: 2, uid: "k", displayName: "Knuckles", connectCode: "", characterId: 0x35, teamId: 0 },
        { port: 3, uid: "l", displayName: "Lucas", connectCode: "", characterId: 0x1b, teamId: 1 },
        { port: 4, uid: "p", displayName: "Peachy", connectCode: "", characterId: 0x0d, teamId: 1 },
      ],
    });
    const a = buildDiscordActivity(
      input({
        game: gameState({ screen: "match", game: twos }),
        queue: { state: "playing", mode: "twos", opponents: [player("l", "Lucas"), PEACHY], game: 2, wins: [0, 1] },
      }),
    );
    expect([a.details, a.state]).toEqual(["2v2 · vs Lucas & Peachy", "with Knuckles · Game 2 · 0 - 1"]);
    expect([a.largeImage, a.smallImage, a.smallText]).toEqual([
      "stage_1",
      "char_mr_game_and_watch",
      "Mr. Game & Watch",
    ]);
  });

  it("shows a room with its name, ruleset, players and Join button", () => {
    const socialRoom: SocialRoom = {
      code: "KFQB",
      players: 3,
      openSlots: 4,
      mode: "ffa",
      status: "waiting",
      joinable: true,
    };
    const room = {
      code: "KFQB",
      name: null,
      host: "Peachy",
      mode: "ffa" as const,
      ruleset: "P+ default",
      players: 3,
      openSlots: 4,
      joinable: true,
    };
    const waiting = buildDiscordActivity(
      input({ game: gameState({ screen: "room" }), socialRoom, room, buttons: true }),
    );
    expect([waiting.details, waiting.state]).toEqual(["Peachy's room", "FFA · P+ default"]);
    expect(waiting.party).toEqual({ size: 3, max: 4 });
    expect(waiting.buttons).toEqual([{ label: "Join their room", url: "https://brawlonline.net/?join=KFQB" }]);

    const inGame = buildDiscordActivity(
      input({
        game: gameState({ screen: "match", game: playing({ mode: "room", gameIndex: 5, stageId: 42 }) }),
        socialRoom: { ...socialRoom, status: "in-game", players: 4, joinable: false },
        room: { ...room, joinable: false },
        buttons: true,
      }),
    );
    expect([inGame.details, inGame.state, inGame.largeText]).toEqual([
      "Peachy's room",
      "FFA · Game 5",
      "Yoshi's Story",
    ]);
    expect(inGame.buttons).toBeUndefined();

    // A private room: no name to show but the mode, and never its code.
    const priv = buildDiscordActivity(
      input({
        game: gameState({ screen: "room" }),
        socialRoom,
        room: { ...room, host: null, joinable: false },
        buttons: true,
      }),
    );
    expect([priv.details, priv.state, priv.buttons]).toEqual(["FFA", "P+ default", undefined]);
  });

  it("shows spectating, replays, Training, Project Punch and Other Games", () => {
    const spectate: SpectateState = {
      status: "watching",
      stream: "0123456789ab",
      entry: {
        id: "0123456789ab",
        mode: "unranked",
        room: null,
        isTeams: false,
        startedAt: "2026-11-24T09:00:00.000Z",
        game: { number: 1, startedAt: "2026-11-24T09:05:00.000Z", stageId: 0x2d, state: "playing" },
        players: [
          { uid: "p", displayName: "Peachy", connectCode: "", characterId: 0x0d, costume: 0, teamId: null, wins: 0 },
          { uid: "k", displayName: "Knuckles", connectCode: "", characterId: 0x35, costume: 0, teamId: null, wins: 0 },
        ],
      },
      spectators: 1,
      autoPlay: false,
      waitingForGame: false,
    };
    const watching = buildDiscordActivity(
      input({ game: gameState({ netplayOpen: false, playbackOpen: true }), spectate }),
    );
    expect([watching.details, watching.state, watching.largeText]).toEqual([
      "Watching",
      "Peachy vs Knuckles",
      "Dream Land",
    ]);
    expect([watching.largeImage, watching.smallImage, watching.smallText]).toEqual([
      "stage_45",
      "watching",
      "Spectate",
    ]);

    const replay = buildDiscordActivity(
      input({
        game: gameState({
          netplayOpen: false,
          playbackOpen: true,
          replay: {
            stageId: 1,
            players: [
              { displayName: "sarahkitty", characterId: 0x14, teamId: null },
              { displayName: "Ferrum", characterId: 0x25, teamId: null },
            ],
          },
        }),
      }),
    );
    expect([replay.details, replay.state]).toEqual(["Watching", "sarahkitty vs Ferrum"]);
    expect([replay.largeImage, replay.smallImage, replay.smallText]).toEqual(["stage_1", "replay", "Replays"]);

    expect(buildDiscordActivity(input({ game: gameState({ screen: "single-player" }) })).details).toBe("Training");
    expect(buildDiscordActivity(input({ game: gameState({ otherGame: "Project Punch" }) })).details).toBe(
      "Project Punch (Training)",
    );
    const other = buildDiscordActivity(input({ game: gameState({ otherGame: "REX" }) }));
    expect([other.details, other.state]).toEqual(["Other Games", "REX"]);
  });

  it("names every rank's badge on the website", () => {
    expect(rankImage({ tier: "legendary", division: null })).toBe("https://brawlonline.net/assets/ranks/legendary.png");
    expect(rankImage({ tier: "master", division: 3 })).toBe("https://brawlonline.net/assets/ranks/master-3.png");
  });
});
