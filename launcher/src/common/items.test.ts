import { describe, expect, it } from "vitest";

import {
  ITEM_SWITCH,
  ITEM_SWITCH_ITEMS,
  ITEM_SWITCH_SETTINGS,
  itemOn,
  itemsOn,
  itemsSummary,
  withAllItems,
  withItem,
} from "./items";
import { PPLUS_ITEM_SWITCH } from "./rulesets";

describe("item switch", () => {
  it("has P+'s 53 cells: 49 items, then 4 settings", () => {
    expect(ITEM_SWITCH).toHaveLength(53);
    expect(ITEM_SWITCH_ITEMS).toHaveLength(49);
    expect(ITEM_SWITCH_SETTINGS.map((e) => [e.bit, e.name])).toEqual([
      [49, "Stage Items"],
      [50, "EX Items"],
      [51, "Mayhem Mode"],
      [52, "Passive-Aggression"],
    ]);
    expect(ITEM_SWITCH.map((e) => e.bit)).toEqual(ITEM_SWITCH.map((_, i) => i));
    expect(ITEM_SWITCH[0].name).toBe("Smash Ball");
    expect(ITEM_SWITCH[36].name).toBe("Smoke Ball");
  });

  it("reads bit i from byte 7 - i / 8 (as the game: P+'s default is Stage Items and EX Items)", () => {
    const on = ITEM_SWITCH.filter((e) => itemOn(PPLUS_ITEM_SWITCH, e.bit)).map((e) => e.name);
    expect(on).toEqual(["Stage Items", "EX Items"]);
    // Smoke Ball toggled on the ITEM SWITCH screen set byte 3 mask 0x10.
    expect(withItem([0, 0, 0, 0, 0, 0, 0, 0], 36, true)).toEqual([0, 0, 0, 0x10, 0, 0, 0, 0]);
    expect(withItem([0, 0, 0, 0, 0, 0, 0, 1], 0, false)).toEqual([0, 0, 0, 0, 0, 0, 0, 0]);
    expect(itemOn([0, 0, 0, 0, 0, 0, 0, 0x80], 7)).toBe(true);
  });

  it("turns every item on or off and leaves the settings", () => {
    const all = withAllItems(PPLUS_ITEM_SWITCH, true);
    expect(itemsOn(all)).toHaveLength(49);
    expect(itemOn(all, 49) && itemOn(all, 50) && !itemOn(all, 51)).toBe(true);
    expect(all).toEqual([0x00, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    expect(withAllItems(all, false)).toEqual(PPLUS_ITEM_SWITCH);
  });

  it("summarizes as P+ plays it", () => {
    expect(itemsSummary(0, withAllItems(PPLUS_ITEM_SWITCH, true))).toBe("none");
    expect(itemsSummary(2, PPLUS_ITEM_SWITCH)).toBe("none");
    expect(itemsSummary(2, withAllItems(PPLUS_ITEM_SWITCH, true))).toBe("all");
    expect(itemsSummary(1, withItem(PPLUS_ITEM_SWITCH, 0, true))).toBe(1);
  });
});
