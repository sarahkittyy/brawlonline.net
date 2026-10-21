import { describe, expect, it } from "vitest";

import { BRAWL_NTSC_U_DISC } from "../game_data";
import { discLabel, matchRoom } from "./room_match";
import type { ModBuild } from "./types";

const DISC = "d".repeat(40);
const OTHER_DISC = "e".repeat(40);
const CARD = "5".repeat(64);
const OTHER_CARD = "6".repeat(64);

function build(id: string, over: Partial<ModBuild> = {}): ModBuild {
  return {
    id,
    name: id,
    discPath: `/games/${id}.iso`,
    launcherPath: null,
    sdPath: `/games/${id}.raw`,
    gameId: "RSBE01",
    revision: 1,
    hash: `${id}`.padEnd(64, "0"),
    parts: { disc: DISC, launcher: null, sd: true, sdCard: CARD },
    hashKey: "k",
    ...over,
  };
}

describe("matchRoom", () => {
  const room = { hash: "h".repeat(64), name: "REX", disc: DISC, discLabel: "RSBE01 Rev 1", sd: CARD };

  it("joins with the same disc and card", () => {
    const m = matchRoom(room, [build("rex")]);
    expect(m).toMatchObject({ joinable: true, disc: "same", sd: "same", hasParts: true });
    expect(m.build?.id).toBe("rex");
  });

  it("joins with another SD card, which is synced first", () => {
    const other = build("old", { parts: { disc: DISC, launcher: null, sd: true, sdCard: OTHER_CARD } });
    const m = matchRoom(room, [other]);
    expect(m).toMatchObject({ joinable: true, disc: "same", sd: "differs" });
  });

  it("prefers the game whose card is the room's", () => {
    const other = build("old", { parts: { disc: DISC, launcher: null, sd: true, sdCard: OTHER_CARD } });
    expect(matchRoom(room, [other, build("rex")]).build?.id).toBe("rex");
  });

  it("prefers the room's exact build when no card is the room's", () => {
    // Bug #28: another release of the mod on the same disc came first, its whole card to fetch.
    const final = build("final", { parts: { disc: DISC, launcher: null, sd: true, sdCard: OTHER_CARD } });
    const hotfix = build("hotfix", {
      hash: room.hash,
      parts: { disc: DISC, launcher: null, sd: true, sdCard: "7".repeat(64) },
    });
    const m = matchRoom(room, [final, hotfix]);
    expect(m).toMatchObject({ joinable: true, disc: "same", sd: "differs" });
    expect(m.build?.id).toBe("hotfix");
  });

  it("does not join with another disc, and names the game to fix", () => {
    const rev2 = build("rev2", { revision: 2, parts: { disc: OTHER_DISC, launcher: null, sd: true, sdCard: CARD } });
    const m = matchRoom(room, [rev2]);
    expect(m).toMatchObject({ joinable: false, disc: "wrong", sd: "same" });
    expect(m.build?.id).toBe("rev2");
  });

  it("does not join without an SD card when the room has one", () => {
    const vanilla = build("vanilla", { sdPath: null, parts: { disc: DISC, launcher: null, sd: false, sdCard: null } });
    expect(matchRoom(room, [vanilla])).toMatchObject({ joinable: false, disc: "same", sd: "wrong" });
  });

  it("joins a room without a card with a game without one", () => {
    const vanilla = build("vanilla", { sdPath: null, parts: { disc: DISC, launcher: null, sd: false, sdCard: null } });
    const m = matchRoom({ ...room, sd: "" }, [build("rex"), vanilla]);
    expect(m).toMatchObject({ joinable: true, disc: "same", sd: null });
    expect(m.build?.id).toBe("vanilla");
  });

  it("compares build hashes for a room from an older launcher", () => {
    const rex = build("rex");
    expect(matchRoom({ hash: rex.hash!, name: "REX" }, [rex])).toMatchObject({ joinable: true, hasParts: false });
    expect(matchRoom({ hash: "x".repeat(64), name: "REX" }, [rex])).toMatchObject({
      joinable: false,
      build: null,
      hasParts: false,
    });
  });

  it("joins a Brawl Rev 2 room with a Rev 1 game: both discs are NTSC-U Brawl", () => {
    const rev1 = build("rex-rev1", {
      revision: 1,
      parts: { disc: BRAWL_NTSC_U_DISC, launcher: null, sd: true, sdCard: CARD },
    });
    const match = matchRoom(
      { hash: "x".repeat(64), name: "REX", disc: BRAWL_NTSC_U_DISC, discLabel: "RSBE01", sd: CARD },
      [rev1],
    );
    expect(match).toMatchObject({ build: rev1, joinable: true, disc: "same", sd: "same" });
  });

  it("has nothing to offer without a game of that kind", () => {
    const zelda = build("zelda", {
      gameId: "RZDE01",
      parts: { disc: OTHER_DISC, launcher: null, sd: false, sdCard: null },
      sdPath: null,
    });
    expect(matchRoom(room, [zelda])).toMatchObject({ build: null, joinable: false, disc: "wrong" });
  });
});

describe("discLabel", () => {
  it("names the game and revision", () => {
    expect(discLabel({ gameId: "RSBE01", revision: 2 })).toBe("RSBE01 Rev 2");
    expect(discLabel({ gameId: "RSBE01" })).toBe("RSBE01 Rev 0");
    expect(discLabel({ gameId: null })).toBe("");
  });

  it("names NTSC-U Brawl without its revision: Rev 1 and Rev 2 are one disc in rooms", () => {
    const parts = { disc: BRAWL_NTSC_U_DISC, launcher: null, sd: false };
    expect(discLabel({ gameId: "RSBE01", revision: 1, parts })).toBe("RSBE01");
    expect(discLabel({ gameId: "RSBE01", revision: 2, parts })).toBe("RSBE01");
  });
});
