// The ITEM SWITCH icons: P+'s `menu2/mu_menumain.pac` holds one 64x64 C8 texture per cell of its ITEM SWITCH screen,
// `MenMainItem.<cell + 1>` (MenMainItem.00 is a 4x4 blank), in the screen's order, which is also the item switch's
// bit order (src/common/items.ts, docs/game-code.md §14). Only P+'s copy: the disc's 49 icons are vanilla Brawl's
// list (no Turbo, no P+ settings) and would put the wrong icon on half the cells.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { ITEM_SWITCH } from "@common/items";

import { decodeTex0 } from "./formats/brres";
import { findTextures } from "./formats/container";
import { encodePng } from "./png";
import type { GameFileSource } from "./sources";

export const ITEM_ICON_ARCHIVE = "menu2/mu_menumain.pac";

export function itemIconTexture(bit: number): string {
  return `MenMainItem.${String(bit + 1).padStart(2, "0")}`;
}

export type ItemIcons = Record<string, { file: string; name: string }>;

/**
 * Writes items/<bit>.png for every ITEM SWITCH cell from the SD card's archive. Returns the manifest entries (keyed by
 * bit) and what was missing.
 */
export async function extractItemIcons(
  sd: GameFileSource | null,
  writeFile: (rel: string, data: Buffer) => Promise<void>,
): Promise<{ items: ItemIcons; missing: string[] }> {
  const items: ItemIcons = {};
  const buf = sd ? await sd.read(ITEM_ICON_ARCHIVE) : null;
  if (!buf) {
    return { items, missing: ["items"] };
  }
  const textures = new Map(findTextures(buf).map((t) => [t.tex.name, t]));
  const missing: string[] = [];
  for (const e of ITEM_SWITCH) {
    const t = textures.get(itemIconTexture(e.bit));
    if (!t) {
      missing.push(`item:${e.bit}`);
      continue;
    }
    const img = decodeTex0(t.tex, t.palette);
    const file = `items/${e.bit}.png`;
    await writeFile(file, encodePng(img.width, img.height, img.rgba));
    items[String(e.bit)] = { file, name: e.name };
  }
  return { items, missing };
}
