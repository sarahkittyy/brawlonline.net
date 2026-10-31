import { describe, expect, it } from "vitest";

import { repeatsStart, SharedStart, STARTING_GRACE_MS } from "./repeat_play";

describe("repeatsStart", () => {
  it("keeps a Dolphin Play started moments ago, with or without the game", () => {
    expect(repeatsStart({ bootsGame: false, at: 1000 }, false, 1200)).toBe(true);
    expect(repeatsStart({ bootsGame: true, at: 1000 }, true, 1000 + STARTING_GRACE_MS - 1)).toBe(true);
  });

  it("restarts one started long enough ago (its game was stopped), or by Configure", () => {
    expect(repeatsStart({ bootsGame: true, at: 1000 }, true, 1000 + STARTING_GRACE_MS)).toBe(false);
    expect(repeatsStart(null, false, 1000)).toBe(false);
  });

  it("restarts one opened without the game when the new Play boots it, and the other way", () => {
    expect(repeatsStart({ bootsGame: false, at: 1000 }, true, 1200)).toBe(false);
    expect(repeatsStart({ bootsGame: true, at: 1000 }, false, 1200)).toBe(false);
  });
});

describe("SharedStart", () => {
  it("gives a call while a start with the same key runs that start's result", async () => {
    const shared = new SharedStart<boolean, string>();
    let starts = 0;
    let finish: (value: string) => void = () => undefined;
    const start = () => {
      starts++;
      return new Promise<string>((resolve) => (finish = resolve));
    };
    let joins = 0;
    const first = shared.run(false, start);
    const second = shared.run(false, start, () => joins++);
    finish("started");
    await expect(first).resolves.toBe("started");
    await expect(second).resolves.toBe("started");
    expect(starts).toBe(1);
    expect(joins).toBe(1);

    // Once it has finished, the next call starts again.
    const third = shared.run(false, start);
    finish("again");
    await expect(third).resolves.toBe("again");
    expect(starts).toBe(2);
  });

  it("starts a call with another key on its own, and passes errors to every caller", async () => {
    const shared = new SharedStart<boolean, string>();
    let fail: (err: Error) => void = () => undefined;
    const failing = shared.run(false, () => new Promise<string>((_resolve, reject) => (fail = reject)));
    const joined = shared.run(false, async () => "never");
    const other = shared.run(true, async () => "booted");
    await expect(other).resolves.toBe("booted");
    fail(new Error("no disc"));
    await expect(failing).rejects.toThrow("no disc");
    await expect(joined).rejects.toThrow("no disc");
    await expect(shared.run(false, async () => "next")).resolves.toBe("next");
  });
});
