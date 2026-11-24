import { describe, expect, it } from "vitest";

import { type BrawlLink, findBrawlLink, parseBrawlLink } from "./brawl_link";

/** A create-room link, or undefined. */
const created = (link: string) => {
  const l = parseBrawlLink(link);
  return l?.kind === "create-room" ? (l as Extract<BrawlLink, { kind: "create-room" }>) : undefined;
};

describe("brawlonline:// links", () => {
  it("reads room/create with a ruleset", () => {
    expect(parseBrawlLink("brawlonline://room/create?ruleset=KFQBTR")).toEqual({
      kind: "create-room",
      ruleset: "KFQBTR",
      isPrivate: false,
    });
    expect(parseBrawlLink("brawlonline://room/create/?ruleset=kfqbtr")).toEqual({
      kind: "create-room",
      ruleset: "KFQBTR",
      isPrivate: false,
    });
    expect(created("BRAWLONLINE://Room/Create?ruleset=KFQBTR&x=1")?.ruleset).toBe("KFQBTR");
    expect(parseBrawlLink("brawlonline://room/create")).toEqual({
      kind: "create-room",
      ruleset: null,
      isPrivate: false,
    });
  });

  it("reads a private create (the website's CREATE PRIVATE ROOM)", () => {
    expect(parseBrawlLink("brawlonline://room/create?ruleset=KFQBTR&private=1")).toEqual({
      kind: "create-room",
      ruleset: "KFQBTR",
      isPrivate: true,
    });
    expect(created("brawlonline://room/create?private=true")?.isPrivate).toBe(true);
    expect(created("brawlonline://room/create?ruleset=KFQBTR&private=0")?.isPrivate).toBe(false);
    expect(created("brawlonline://room/create?ruleset=KFQBTR&private=")?.isPrivate).toBe(false);
  });

  it("drops everything else", () => {
    for (const bad of [
      "brawlonline://room/create?ruleset=KFQB",
      "brawlonline://room/create?ruleset=AEIOUY",
      "brawlonline://room/join?code=KFQA",
      "brawlonline://room/join",
      "brawlonline://watch?stream=0123456789",
      "brawlonline://watch?stream=0123456789xy",
      "brawlonline://ruleset/KFQBTR",
      "brawlonline:",
      "https://brawlonline.net/r/KFQBTR",
      "slippi://room/create?ruleset=KFQBTR",
      "not a url",
    ]) {
      expect(parseBrawlLink(bad), bad).toBeNull();
    }
  });

  it("reads the Discord activity's room/join and watch (through the website's /?join= and /?watch=)", () => {
    expect(parseBrawlLink("brawlonline://room/join?code=KFQB")).toEqual({ kind: "join-room", code: "KFQB" });
    expect(parseBrawlLink("brawlonline://room/join/?code=kfqb")).toEqual({ kind: "join-room", code: "KFQB" });
    expect(parseBrawlLink("brawlonline://watch?stream=0123456789AB")).toEqual({
      kind: "watch",
      stream: "0123456789ab",
    });
  });

  it("finds a link on a command line", () => {
    expect(findBrawlLink(["Brawl Online.exe", "--flag", "brawlonline://room/create?ruleset=KFQBTR"])).toBe(
      "brawlonline://room/create?ruleset=KFQBTR",
    );
    expect(findBrawlLink(["Brawl Online.exe", "C:\replaysa.rep"])).toBeUndefined();
  });
});
