import { describe, expect, it } from "vitest";

import { createProgressThrottle, runWithProgress } from "./setup_progress";
import { DolphinLaunchType } from "./types";

describe("createProgressThrottle", () => {
  it("sends the first, the last, and one value per interval, per launch type", () => {
    let now = 0;
    const send = createProgressThrottle(100, () => now);
    const sent: number[] = [];
    for (let i = 0; i <= 20; i++) {
      if (send(DolphinLaunchType.NETPLAY, i, 20, "prepareSdCard")) {
        sent.push(i);
      }
      now += 25;
    }
    expect(sent).toEqual([0, 4, 8, 12, 16, 20]);
    // Another launch type has its own clock.
    expect(send(DolphinLaunchType.PLAYBACK, 5, 20, "installDolphin")).toBe(true);
  });

  it("always sends the first value of a new phase", () => {
    let now = 0;
    const send = createProgressThrottle(100, () => now);
    expect(send(DolphinLaunchType.NETPLAY, 10, 100, "downloadProjectPlus")).toBe(true);
    now += 1;
    expect(send(DolphinLaunchType.NETPLAY, 20, 100, "downloadProjectPlus")).toBe(false);
    expect(send(DolphinLaunchType.NETPLAY, 3, 100, "verifyProjectPlus")).toBe(true);
    expect(send(DolphinLaunchType.NETPLAY, 4, 100, "verifyProjectPlus")).toBe(false);
    expect(send(DolphinLaunchType.NETPLAY, 100, 100, "verifyProjectPlus")).toBe(true);
  });
});

describe("runWithProgress", () => {
  const recorder = () => {
    const events: string[] = [];
    return {
      events,
      handlers: {
        start: () => events.push("start"),
        progress: (c: number, t: number) => events.push(`${c}/${t}`),
        complete: () => events.push("complete"),
      },
    };
  };

  it("shows a step from its first report until it ends", async () => {
    const { events, handlers } = recorder();
    const result = await runWithProgress(async (onProgress) => {
      onProgress(0, 2);
      onProgress(1, 2);
      onProgress(2, 2);
      return "done";
    }, handlers);
    expect(result).toBe("done");
    expect(events).toEqual(["start", "0/2", "1/2", "2/2", "complete"]);
  });

  it("leaves the button alone when the step has nothing to do", async () => {
    const { events, handlers } = recorder();
    await runWithProgress(async () => "unchanged", handlers);
    expect(events).toEqual([]);
  });

  it("makes the button ready again when the step fails", async () => {
    const { events, handlers } = recorder();
    await expect(
      runWithProgress(async (onProgress) => {
        onProgress(0, 10);
        throw new Error("disk full");
      }, handlers),
    ).rejects.toThrow("disk full");
    expect(events).toEqual(["start", "0/10", "complete"]);
  });
});
