// The stage select's icons: P+'s `menu2/sc_selmap.pac` holds one 128x112 C8 texture per stage, `MenSelmapIcon.<icon>`
// (src/common/stages.ts lists each stage's icon). Only P+'s copy: the disc's are vanilla Brawl's stages under other
// numbers.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { PPLUS_STAGES, stageIconTexture } from "@common/stages";

import { decodeTex0 } from "./formats/brres";
import { findTextures } from "./formats/container";
import { encodePng } from "./png";
import type { GameFileSource } from "./sources";

export const STAGE_ICON_ARCHIVE = "menu2/sc_selmap.pac";

export type StageIcons = Record<string, { file: string; name: string }>;

/**
 * Writes stages/<id>.png for every P+ stage from the SD card's archive. Returns the manifest entries (keyed by stage
 * id) and what was missing.
 */
export async function extractStageIcons(
  sd: GameFileSource | null,
  writeFile: (rel: string, data: Buffer) => Promise<void>,
): Promise<{ stages: StageIcons; missing: string[] }> {
  const stages: StageIcons = {};
  const buf = sd ? await sd.read(STAGE_ICON_ARCHIVE) : null;
  if (!buf) {
    return { stages, missing: ["stages"] };
  }
  // The archive also has 4x4 placeholders under some of the names.
  const textures = new Map(
    findTextures(buf)
      .filter((t) => t.tex.width > 4)
      .map((t) => [t.tex.name, t]),
  );
  const missing: string[] = [];
  for (const s of PPLUS_STAGES) {
    const t = textures.get(stageIconTexture(s.icon));
    if (!t) {
      missing.push(`stage:${s.id}`);
      continue;
    }
    const img = decodeTex0(t.tex, t.palette);
    const file = `stages/${s.id}.png`;
    await writeFile(file, encodePng(img.width, img.height, img.rgba));
    stages[String(s.id)] = { file, name: s.name };
  }
  return { stages, missing };
}
