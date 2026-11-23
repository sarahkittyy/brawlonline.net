import { describe, expect, it } from "vitest";

import { badgesFor, iconUrl, isIconId, parseBadges } from "./badges";

const ID = "0123456789abcdef0123";

describe("badges", () => {
  it("takes only icon ids", () => {
    expect(isIconId(ID)).toBe(true);
    expect(isIconId("../../v1/me00000000")).toBe(false);
    expect(isIconId(ID.toUpperCase())).toBe(false);
    expect(iconUrl("https://brawlonline.net", ID)).toBe(`https://brawlonline.net/v1/icons/${ID}.png`);
  });

  it("checks what the server sends", () => {
    expect(parseBadges(undefined)).toBeUndefined();
    expect(parseBadges([{ icon: "nope" }])).toBeUndefined();
    expect(
      parseBadges([{ icon: ID, label: "Dev", places: ["chat", "spectate"] }, { icon: "x" }, null, { icon: ID }]),
    ).toEqual([{ icon: ID, label: "Dev", places: ["chat"] }, { icon: ID }]);
    expect(parseBadges(Array.from({ length: 20 }, () => ({ icon: ID })))?.length).toBe(8);
  });

  it("shows a badge where it is listed, or everywhere", () => {
    const everywhere = { icon: ID };
    const chat = { icon: ID, places: ["chat" as const] };
    expect(badgesFor([everywhere, chat], "chat")).toEqual([everywhere, chat]);
    expect(badgesFor([everywhere, chat], "profile")).toEqual([everywhere]);
    expect(badgesFor(undefined, "online")).toEqual([]);
  });
});
