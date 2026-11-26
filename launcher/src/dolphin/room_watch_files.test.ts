import { mkdtemp, readFile, rm, stat, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import {
  parseRoomWatchClips,
  ROOM_WATCH_CLIPS_FILE,
  ROOM_WATCH_OVERLAY_FILE,
  RoomWatchFiles,
} from "./room_watch_files";
import type { PlaybackOverlayFile, ReplayPlaybackStatus } from "./types";

const CLIPS = {
  version: 1,
  run: "1764134400000",
  savedClips: [{ path: "C:/U/Online/RoomWatch/ab.brep", startFrame: 600, endFrame: 1800 }],
  renderRequests: [
    {
      seq: 1,
      path: "C:/U/Online/RoomWatch/ab.brep",
      startFrame: 600,
      endFrame: 1800,
      music: true,
      clip: true,
      label: "Clip 0:10 - 0:30 (20 s)",
    },
  ],
  showRender: { seq: 2, id: "job-1" },
};

describe("parseRoomWatchClips", () => {
  it("reads the game's clips file as a playback status", () => {
    expect(parseRoomWatchClips(JSON.stringify(CLIPS))).toEqual({
      run: CLIPS.run,
      savedClips: CLIPS.savedClips,
      renderRequests: CLIPS.renderRequests,
      showRender: CLIPS.showRender,
    });
  });

  it("leaves out what isn't a clip, and refuses another file", () => {
    const odd = {
      ...CLIPS,
      savedClips: [null, { path: 3, startFrame: 1, endFrame: 2 }, { path: "x", startFrame: -1, endFrame: 2 }],
      renderRequests: [{ seq: 1, path: "x", startFrame: 1, endFrame: 2 }],
      showRender: { seq: "2", id: "job" },
    };
    expect(parseRoomWatchClips(JSON.stringify(odd))).toEqual({ run: CLIPS.run, savedClips: [], renderRequests: [] });
    expect(parseRoomWatchClips(JSON.stringify({ ...CLIPS, version: 2 }))).toBeNull();
    expect(parseRoomWatchClips(JSON.stringify({ ...CLIPS, run: 5 }))).toBeNull();
    expect(parseRoomWatchClips("{")).toBeNull();
    expect(parseRoomWatchClips("[]")).toBeNull();
  });
});

describe("RoomWatchFiles", () => {
  let dir: string;
  let open: boolean;
  let overlay: PlaybackOverlayFile | null;
  let statuses: ReplayPlaybackStatus[];
  let files: RoomWatchFiles;

  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "room-watch-"));
    open = true;
    overlay = { accent: "#3366ff", clip: "#ff9933", toasts: [] };
    statuses = [];
    files = new RoomWatchFiles({
      onlineDir: () => (open ? dir : null),
      overlay: () => overlay,
      onStatus: (s) => statuses.push(s),
    });
  });

  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("hands on each new clips file once, and writes the seek bar's side when it changes", async () => {
    await files.tick();
    expect(statuses).toEqual([]);
    expect(JSON.parse(await readFile(path.join(dir, ROOM_WATCH_OVERLAY_FILE), "utf8"))).toEqual(overlay);

    await writeFile(path.join(dir, ROOM_WATCH_CLIPS_FILE), JSON.stringify(CLIPS));
    await files.tick();
    await files.tick();
    expect(statuses).toHaveLength(1);
    expect(statuses[0].savedClips).toEqual(CLIPS.savedClips);

    const more = {
      ...CLIPS,
      savedClips: [...CLIPS.savedClips, { ...CLIPS.savedClips[0], startFrame: 2000, endFrame: 2600 }],
    };
    await writeFile(path.join(dir, ROOM_WATCH_CLIPS_FILE), JSON.stringify(more));
    overlay = { toasts: [] };
    await files.tick();
    expect(statuses).toHaveLength(2);
    expect(statuses[1].savedClips).toHaveLength(2);
    expect(JSON.parse(await readFile(path.join(dir, ROOM_WATCH_OVERLAY_FILE), "utf8"))).toEqual({ toasts: [] });

    overlay = null;
    await files.tick();
    await expect(stat(path.join(dir, ROOM_WATCH_OVERLAY_FILE))).rejects.toThrow();
  });

  it("does nothing while no netplay game is open", async () => {
    open = false;
    await writeFile(path.join(dir, ROOM_WATCH_CLIPS_FILE), JSON.stringify(CLIPS));
    await files.tick();
    expect(statuses).toEqual([]);
    await expect(stat(path.join(dir, ROOM_WATCH_OVERLAY_FILE))).rejects.toThrow();
  });
});
