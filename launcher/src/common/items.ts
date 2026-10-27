/**
 * P+'s ITEM SWITCH (`MatchRules.itemSwitch`, the menu record's 8 bytes at +0x818): what each bit is.
 *
 * Read off P+ v3.2 itself (2026-10-11, docs/game-code.md §14 "Item switch layout"): the ITEM SWITCH
 * screen is a grid of 53 cells after ALL, in reading order; cell `i` is bit `i` of the 8 bytes read
 * as one big-endian number (byte `7 - i / 8`, mask `1 << i % 8`). Bit patterns written into the
 * page's working copy and the screen's ON / OFF labels gave each cell's bit; hovering each cell
 * gave its name. Its icon is `MenMainItem.<i + 1>` in P+'s `menu2/mu_menumain.pac`.
 *
 * Cells 49-52 are not items but P+ settings kept in the same field ("Setting" on the screen):
 * Stage Items and EX Items are on in P+'s default (`00 06 00 00 00 00 00 00`).
 */

export type ItemKind = "item" | "setting";

export type ItemSwitchEntry = {
  /** The bit (and the cell's position on the ITEM SWITCH screen). */
  bit: number;
  name: string;
  kind: ItemKind;
  /** P+'s category line ("Throwing", "Battering"...). */
  category: string;
};

const ROWS: [string, string][] = [
  ["Smash Ball", "Final Smash"],
  ["Assist Trophy", "Helping"],
  ["Poké Ball", "Helping"],
  ["Container", "Carrying"],
  ["Blast Box", "Exploding"],
  ["Sandbag", "Prize Giving"],
  ["Food", "Recovery"],
  ["Maxim Tomato", "Recovery"],
  ["Heart Container", "Recovery"],
  ["Dragoon Parts", "Combining"],
  ["Super Mushroom", "Transforming"],
  ["Poison Mushroom", "Transforming"],
  ["Warp Star", "Transforming"],
  ["Starman", "Transforming"],
  ["Metal Box", "Transforming"],
  ["Bunny Hood", "Transforming"],
  ["Turbo", "Transforming"],
  ["Timer", "Transforming"],
  ["Lightning", "Transforming"],
  ["Beam Sword", "Battering"],
  ["Home-Run Bat", "Battering"],
  ["Fan", "Battering"],
  ["Lip's Stick", "Battering"],
  ["Star Rod", "Battering"],
  ["Hammer", "Battering"],
  ["Golden Hammer", "Battering"],
  ["Ray Gun", "Shooting"],
  ["Super Scope", "Shooting"],
  ["Fire Flower", "Shooting"],
  ["Cracker Launcher", "Shooting"],
  ["Bob-omb", "Throwing"],
  ["Motion-Sensor Bomb", "Throwing"],
  ["Gooey Bomb", "Throwing"],
  ["Smart Bomb", "Throwing"],
  ["Deku Nut", "Throwing"],
  ["Freezie", "Throwing"],
  ["Smoke Ball", "Throwing"],
  ["Pitfall", "Throwing"],
  ["Hothead", "Throwing"],
  ["Mr. Saturn", "Throwing"],
  ["Green Shell", "Throwing"],
  ["Banana Peel", "Throwing"],
  ["Bumper", "Throwing"],
  ["Spring", "Throwing"],
  ["Unira", "Throwing"],
  ["Soccer Ball", "Throwing"],
  ["Team Healer", "Throwing"],
  ["Franklin Badge", "Gear"],
  ["Screw Attack", "Gear"],
  ["Stage Items", "Setting"],
  ["EX Items", "Setting"],
  ["Mayhem Mode", "Setting"],
  ["Passive-Aggression", "Setting"],
];

/** Every cell of P+'s ITEM SWITCH, in the screen's order (= bit order). */
export const ITEM_SWITCH: readonly ItemSwitchEntry[] = ROWS.map(([name, category], bit) => ({
  bit,
  name,
  category,
  kind: category === "Setting" ? "setting" : "item",
}));

export const ITEM_SWITCH_ITEMS = ITEM_SWITCH.filter((e) => e.kind === "item");
export const ITEM_SWITCH_SETTINGS = ITEM_SWITCH.filter((e) => e.kind === "setting");

/** P+'s ITEM SWITCH frequency line: Brawl's four, then P+'s VERY HIGH, INTENSE and BOMB RAIN (sudden death's bombs). */
export const ITEM_FREQUENCIES = ["None", "Low", "Medium", "High", "Very High", "Intense", "Bomb Rain"] as const;

export function itemOn(itemSwitch: readonly number[], bit: number): boolean {
  return ((itemSwitch[7 - (bit >> 3)] ?? 0) & (1 << (bit & 7))) !== 0;
}

/** A copy of the 8 bytes with `bit` set to `on`. */
export function withItem(itemSwitch: readonly number[], bit: number, on: boolean): number[] {
  const out = Array.from({ length: 8 }, (_, i) => itemSwitch[i] ?? 0);
  const byte = 7 - (bit >> 3);
  const mask = 1 << (bit & 7);
  out[byte] = on ? out[byte] | mask : out[byte] & ~mask;
  return out;
}

/** Every item (not the settings) on or off; the settings stay as they are. */
export function withAllItems(itemSwitch: readonly number[], on: boolean): number[] {
  let out = Array.from({ length: 8 }, (_, i) => itemSwitch[i] ?? 0);
  for (const e of ITEM_SWITCH_ITEMS) {
    out = withItem(out, e.bit, on);
  }
  return out;
}

/** The items (not the settings) that are on. */
export function itemsOn(itemSwitch: readonly number[]): ItemSwitchEntry[] {
  return ITEM_SWITCH_ITEMS.filter((e) => itemOn(itemSwitch, e.bit));
}

/**
 * Items in a match: none when the frequency is None or every item is off (P+ treats both as items
 * off: "Item Frequency None if all items turned off", RSBE01.txt), "all", or a count.
 */
export function itemsSummary(frequency: number, itemSwitch: readonly number[]): "none" | "all" | number {
  const on = itemsOn(itemSwitch).length;
  if (frequency === 0 || on === 0) {
    return "none";
  }
  return on === ITEM_SWITCH_ITEMS.length ? "all" : on;
}
