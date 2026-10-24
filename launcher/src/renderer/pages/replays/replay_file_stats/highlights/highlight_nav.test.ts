import { beforeEach, describe, expect, it, vi } from "vitest";

import type { PlaylistEntry } from "./highlight_nav";
import {
  followStatus,
  listView,
  navOrder,
  navTarget,
  REPLAY_LIST,
  replayEntry,
  replayListNav,
  usePlaylistStore,
} from "./highlight_nav";

const e = (path: string, startFrame: number, endFrame = startFrame + 300): PlaylistEntry => ({
  key: `${path}@${startFrame}`,
  path,
  startFrame,
  endFrame,
});

// A list sorted best first: replays a, b, c interleaved.
const A1 = e("a", 1000);
const A2 = e("a", 5000);
const A3 = e("a", 9000);
const B1 = e("b", 2000);
const B2 = e("b", 7000);
const C1 = e("c", 300);
const LIST = [A2, B2, A1, C1, B1, A3];

describe("the list's order for the buttons", () => {
  it("takes the replays as they first come, each one's highlights by their start", () => {
    expect(navOrder(LIST)).toEqual([[A1, A2, A3], [B1, B2], [C1]]);
  });

  it("gives Dolphin this replay's stars with their places, and the count", () => {
    expect(listView(LIST, B2)).toEqual({
      marks: [
        { startFrame: 2000, endFrame: 2300, position: 4 },
        { startFrame: 7000, endFrame: 7300, position: 5 },
      ],
      nav: { position: 5, total: 6, hasPrevReplay: true, hasNextReplay: true },
    });
    expect(listView(LIST, A1).nav).toEqual({ position: 1, total: 6, hasPrevReplay: false, hasNextReplay: true });
    expect(listView(LIST, C1).nav).toEqual({ position: 6, total: 6, hasPrevReplay: true, hasNextReplay: false });
    // Not in the list (another section opened): no place, no stars, both ways open.
    expect(listView(LIST, e("z", 10))).toEqual({
      marks: [],
      nav: { position: 0, total: 6, hasPrevReplay: true, hasNextReplay: true },
    });
    // A manual clip of a listed replay that isn't in the section: its replay's stars, no place.
    expect(listView(LIST, e("a", 4000)).nav.position).toBe(0);
    expect(listView(LIST, e("a", 4000)).marks).toHaveLength(3);
  });
});

describe("where a button goes", () => {
  it("goes clip by clip from the frame shown, then on to the next replay", () => {
    expect(navTarget(LIST, { path: "a", frame: 1100 }, "nextClip")).toBe(A2);
    expect(navTarget(LIST, { path: "a", frame: 9100 }, "nextClip")).toBe(B1);
    expect(navTarget(LIST, { path: "c", frame: 400 }, "nextClip")).toBeNull();
  });

  it("plays the one shown again from its start, unless just started; then the replay before's last", () => {
    expect(navTarget(LIST, { path: "a", frame: 5500 }, "prevClip")).toBe(A2);
    expect(navTarget(LIST, { path: "a", frame: 5030 }, "prevClip")).toBe(A1);
    expect(navTarget(LIST, { path: "b", frame: 2010 }, "prevClip")).toBe(A3);
    expect(navTarget(LIST, { path: "a", frame: 1010 }, "prevClip")).toBeNull();
  });

  it("goes file by file to a replay's first highlight", () => {
    expect(navTarget(LIST, { path: "a", frame: 9100 }, "nextReplay")).toBe(B1);
    expect(navTarget(LIST, { path: "c", frame: 0 }, "prevReplay")).toBe(B1);
    expect(navTarget(LIST, { path: "a", frame: 0 }, "prevReplay")).toBeNull();
    expect(navTarget(LIST, { path: "c", frame: 0 }, "nextReplay")).toBeNull();
  });

  it("starts from the list's ends for a replay not in it", () => {
    expect(navTarget(LIST, { path: "z", frame: 0 }, "nextClip")).toBe(A1);
    expect(navTarget(LIST, { path: "z", frame: 0 }, "nextReplay")).toBe(A1);
    expect(navTarget(LIST, { path: "z", frame: 0 }, "prevClip")).toBe(C1);
    expect(navTarget(LIST, { path: "z", frame: 0 }, "prevReplay")).toBe(C1);
    expect(navTarget([], { path: "z", frame: 0 }, "nextClip")).toBeNull();
  });
});

describe("following Dolphin", () => {
  beforeEach(() => {
    usePlaylistStore.setState({
      source: "highlights",
      entries: LIST,
      current: A1,
      commandId: "hl-1",
      navSeq: 0,
      pickSeq: 0,
    });
  });

  it("plays what a list button asks for once, from the list", () => {
    const play = vi.fn();
    const status = {
      commandId: "hl-1",
      replay: "a",
      navRequest: { seq: 1, action: "nextReplay" as const, frame: 1200 },
    };
    followStatus(status, play);
    followStatus(status, play);
    expect(play).toHaveBeenCalledTimes(1);
    expect(play).toHaveBeenCalledWith("highlights", LIST, B1);
    // An older request's status: nothing.
    followStatus({ ...status, commandId: "hl-0", navRequest: { ...status.navRequest, seq: 2 } }, play);
    expect(play).toHaveBeenCalledTimes(1);
  });

  it("makes a star Dolphin played the row playing", () => {
    followStatus(
      { commandId: "hl-1+anyway", replay: "a", highlightPick: { seq: 1, startFrame: 9000, endFrame: 9300 } },
      vi.fn(),
    );
    expect(usePlaylistStore.getState().current).toBe(A3);
    expect(usePlaylistStore.getState().commandId).toBe("hl-1+anyway");
    // A clip saved there, not in the list: nothing of the list plays.
    followStatus(
      { commandId: "hl-1+anyway", replay: "a", highlightPick: { seq: 2, startFrame: 10, endFrame: 400 } },
      vi.fn(),
    );
    expect(usePlaylistStore.getState().current).toMatchObject({ key: "", path: "a", startFrame: 10 });
  });
});

describe("the Replays tab's list", () => {
  const REPLAYS = ["r1", "r2", "r3"].map(replayEntry);

  it("counts its replays, << >> only", () => {
    expect(replayListNav(REPLAYS, "r1")).toEqual({
      position: 1,
      total: 3,
      hasPrevReplay: false,
      hasNextReplay: true,
      clips: false,
    });
    expect(replayListNav(REPLAYS, "r3")).toMatchObject({ position: 3, hasPrevReplay: true, hasNextReplay: false });
    expect(replayListNav(REPLAYS, "zz")).toMatchObject({ position: 0, hasPrevReplay: true, hasNextReplay: true });
  });

  it("plays the replay before or after the one playing", () => {
    usePlaylistStore.setState({
      source: REPLAY_LIST,
      entries: REPLAYS,
      current: REPLAYS[1],
      commandId: "hl-2",
      navSeq: 0,
      pickSeq: 0,
    });
    const play = vi.fn();
    followStatus({ commandId: "hl-2", replay: "r2", navRequest: { seq: 1, action: "nextReplay", frame: 900 } }, play);
    expect(play).toHaveBeenCalledWith(REPLAY_LIST, REPLAYS, REPLAYS[2]);
    followStatus({ commandId: "hl-2", replay: "r2", navRequest: { seq: 2, action: "prevReplay", frame: 900 } }, play);
    expect(play).toHaveBeenLastCalledWith(REPLAY_LIST, REPLAYS, REPLAYS[0]);
  });
});
