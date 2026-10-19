import { describe, expect, it, vi } from "vitest";

import { checkRoomCard, parseSdCompare } from "./sd_listing";

const ROOT_A = "a".repeat(64);
const ROOT_B = "b".repeat(64);
const differs = {
  kind: "differs" as const,
  bytes: 2 * 1024 * 1024,
  files: 31,
  names: ["rex_/a.pac"],
  filesKnown: true,
};

describe("parseSdCompare", () => {
  it("reads DolphinTool sdcompare's answer", () => {
    expect(
      parseSdCompare('{"bytes":1966080,"files":31,"filesKnown":true,"names":["REX_\\/PF\\/MOVIE\\/OPMovie.thp"]}\n'),
    ).toEqual({ kind: "differs", bytes: 1966080, files: 31, names: ["REX_/PF/MOVIE/OPMovie.thp"], filesKnown: true });
    expect(parseSdCompare('{"bytes":10,"files":0,"filesKnown":false,"names":[]}')?.filesKnown).toBe(false);
  });

  it("refuses anything else", () => {
    expect(parseSdCompare("")).toBeNull();
    expect(parseSdCompare("Error: the listing is of another card")).toBeNull();
    expect(parseSdCompare('{"bytes":"1","files":2,"names":[]}')).toBeNull();
    expect(parseSdCompare("[]")).toBeNull();
  });
});

describe("checkRoomCard", () => {
  it("asks nothing when the cards are the same, or one side has none", async () => {
    const fetchListing = vi.fn(async () => new Uint8Array([1]));
    const compare = vi.fn(async () => differs);
    expect(await checkRoomCard({ ownRoot: ROOT_A, roomRoot: ROOT_A, fetchListing, compare })).toEqual({ kind: "same" });
    expect(await checkRoomCard({ ownRoot: null, roomRoot: ROOT_A, fetchListing, compare })).toEqual({ kind: "same" });
    expect(await checkRoomCard({ ownRoot: ROOT_A, roomRoot: "", fetchListing, compare })).toEqual({ kind: "same" });
    expect(fetchListing).not.toHaveBeenCalled();
  });

  it("names what differs from the host's listing", async () => {
    const listing = new Uint8Array([9, 9]);
    const compare = vi.fn(async () => differs);
    expect(
      await checkRoomCard({ ownRoot: ROOT_A, roomRoot: ROOT_B, fetchListing: async () => listing, compare }),
    ).toEqual(differs);
    expect(compare).toHaveBeenCalledWith(listing);
  });

  it("asks without sizes when the listing is missing or unusable", async () => {
    const log = vi.fn();
    const compare = vi.fn(async () => differs);
    // A host on an older launcher: nothing on the server.
    expect(await checkRoomCard({ ownRoot: ROOT_A, roomRoot: ROOT_B, fetchListing: async () => null, compare })).toEqual(
      { kind: "unknown" },
    );
    // The server unreachable.
    const failing = async () => {
      throw new Error("network");
    };
    expect(await checkRoomCard({ ownRoot: ROOT_A, roomRoot: ROOT_B, fetchListing: failing, compare, log })).toEqual({
      kind: "unknown",
    });
    // A listing DolphinTool refuses (another card's, malformed).
    const refusing = async () => {
      throw new Error("Error: the listing is of another card");
    };
    expect(
      await checkRoomCard({
        ownRoot: ROOT_A,
        roomRoot: ROOT_B,
        fetchListing: async () => new Uint8Array([1]),
        compare: refusing,
        log,
      }),
    ).toEqual({ kind: "unknown" });
    expect(log).toHaveBeenCalledTimes(2);
  });

  it("asks nothing when nothing would download", async () => {
    const none = { ...differs, bytes: 0, files: 0, names: [] };
    expect(
      await checkRoomCard({
        ownRoot: ROOT_A,
        roomRoot: ROOT_B,
        fetchListing: async () => new Uint8Array([1]),
        compare: async () => none,
      }),
    ).toEqual({ kind: "same" });
  });
});
