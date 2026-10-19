import { mkdir, mkdtemp, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { closeIfIdle, QUIT_IF_IDLE_ANSWER_FILE, QUIT_IF_IDLE_FILE } from "./quit_if_idle";
import { GAME_STATUS_FILE } from "./room_handoff";

/**
 * A fake Dolphin main window: every poll it reads quit-if-idle.json as MainWindow's timer does,
 * deletes it, answers (or not, as an older build) and, without a game, closes `closeAfterPolls`
 * polls later.
 */
function fakeDolphin(
  dir: string,
  opts: { inGame: boolean; answers?: boolean; closeAfterPolls?: number; neverCloses?: boolean },
) {
  let open = true;
  let closingIn: number | null = null;
  let clock = 0;
  const seen: unknown[] = [];
  const sleep = async (ms: number) => {
    clock += ms;
    if (closingIn != null && --closingIn <= 0) {
      open = false;
    }
    const file = path.join(dir, QUIT_IF_IDLE_FILE);
    let text: string;
    try {
      text = await readFile(file, "utf8");
    } catch {
      return;
    }
    await rm(file);
    const request = JSON.parse(text);
    seen.push(request);
    if (opts.answers !== false) {
      const answer = { version: 1, id: request.id, result: opts.inGame ? "in-game" : "closing" };
      await writeFile(path.join(dir, QUIT_IF_IDLE_ANSWER_FILE), JSON.stringify(answer));
    }
    if (!opts.inGame && !opts.neverCloses) {
      closingIn = opts.closeAfterPolls ?? 1;
    }
  };
  return { isOpen: () => open, sleep, now: () => clock, seen, elapsed: () => clock };
}

const exists = (file: string) =>
  readFile(file).then(
    () => true,
    () => false,
  );

describe("quit if idle", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "quit-if-idle-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("closes a Dolphin whose game was stopped, though its status still looks fresh", async () => {
    // The rooms thread keeps writing the status after Stop, with the last screen the game showed.
    await writeFile(
      path.join(dir, GAME_STATUS_FILE),
      JSON.stringify({ version: 1, state: "idle", screen: "menus", updatedAt: new Date().toISOString() }),
    );
    const dolphin = fakeDolphin(dir, { inGame: false });
    expect(await closeIfIdle(dir, dolphin.isOpen, dolphin)).toBe(true);
    expect(dolphin.isOpen()).toBe(false);
    expect(dolphin.seen).toHaveLength(1);
  });

  it("answers at once for a Dolphin in a game, which stays open", async () => {
    const dolphin = fakeDolphin(dir, { inGame: true });
    expect(await closeIfIdle(dir, dolphin.isOpen, dolphin)).toBe(false);
    expect(dolphin.isOpen()).toBe(true);
    // One poll, not the whole wait.
    expect(dolphin.elapsed()).toBeLessThan(500);
    expect(await exists(path.join(dir, QUIT_IF_IDLE_FILE))).toBe(false);
    expect(await exists(path.join(dir, QUIT_IF_IDLE_ANSWER_FILE))).toBe(false);
  });

  it("waits longer than the answer timeout for a Dolphin that said it is closing", async () => {
    // Leaving a room and logging out: 6 s.
    const dolphin = fakeDolphin(dir, { inGame: false, closeAfterPolls: 60 });
    expect(await closeIfIdle(dir, dolphin.isOpen, { ...dolphin, answerTimeoutMs: 4000 })).toBe(true);
    expect(dolphin.elapsed()).toBeGreaterThan(4000);
  });

  it("gives up on a Dolphin that said it is closing but never closes", async () => {
    const dolphin = fakeDolphin(dir, { inGame: false, neverCloses: true });
    expect(await closeIfIdle(dir, dolphin.isOpen, { ...dolphin, closingTimeoutMs: 2000 })).toBe(false);
    expect(dolphin.elapsed()).toBeLessThan(2500);
  });

  it("an older Dolphin without the answer: closes without a game, stays in one after the wait", async () => {
    const idle = fakeDolphin(dir, { inGame: false, answers: false });
    expect(await closeIfIdle(dir, idle.isOpen, idle)).toBe(true);
    const inGame = fakeDolphin(dir, { inGame: true, answers: false });
    expect(await closeIfIdle(dir, inGame.isOpen, { ...inGame, answerTimeoutMs: 1000 })).toBe(false);
    expect(inGame.elapsed()).toBeGreaterThanOrEqual(1000);
  });

  it("ignores an answer left from an earlier request", async () => {
    await writeFile(
      path.join(dir, QUIT_IF_IDLE_ANSWER_FILE),
      JSON.stringify({ version: 1, id: "earlier", result: "in-game" }),
    );
    const dolphin = fakeDolphin(dir, { inGame: false });
    expect(await closeIfIdle(dir, dolphin.isOpen, dolphin)).toBe(true);
  });

  it("asks nothing when no Dolphin is open", async () => {
    const dolphin = fakeDolphin(dir, { inGame: true });
    expect(await closeIfIdle(dir, () => false, dolphin)).toBe(true);
    expect(dolphin.seen).toHaveLength(0);
    expect(await exists(path.join(dir, QUIT_IF_IDLE_FILE))).toBe(false);
  });

  it("asks only the Dolphin whose online folder it is given (a replay's Dolphin has its own)", async () => {
    const playbackDir = path.join(dir, "playback");
    const netplayDir = path.join(dir, "netplay");
    await Promise.all([playbackDir, netplayDir].map((d) => mkdir(d)));
    const replay = fakeDolphin(playbackDir, { inGame: true });
    const game = fakeDolphin(netplayDir, { inGame: false });
    const sleep = async (ms: number) => {
      await replay.sleep(0);
      await game.sleep(ms);
    };
    expect(await closeIfIdle(netplayDir, game.isOpen, { sleep, now: game.now })).toBe(true);
    expect(replay.seen).toHaveLength(0);
    expect(replay.isOpen()).toBe(true);
  });
});
