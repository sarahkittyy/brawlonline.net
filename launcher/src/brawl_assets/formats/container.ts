// Recursive view over nested Brawl containers: compressed streams, ARC, U8 and BRRES.
//
// Every node has a slash-separated path made of ARC entry names (`MiscData[0]`), U8 paths, and BRRES
// sub-file names (`Textures(NW4R)/MenMainBtn01`), e.g. `MiscData[0]/Textures(NW4R)/MenMainBtn01`.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { isArc, parseArc } from "./arc";
import type { BrresTexture } from "./brres";
import { isBrres, listBrresTextures, parseBrres } from "./brres";
import { decompress, detectCompression } from "./lz77";
import { isRfnt } from "./rfnt";
import { isU8, parseU8 } from "./u8";

export type NodeKind = "arc" | "u8" | "brres" | "rfnt" | "data";

export type ContainerNode = {
  path: string;
  kind: NodeKind;
  /** Decompressed bytes. */
  data: Buffer;
  compressed: boolean;
};

/** Decompresses `buf` if it is a Nintendo-compressed stream whose payload is a recognised container. */
export function unwrap(buf: Buffer): { data: Buffer; compressed: boolean } {
  if (detectCompression(buf)) {
    try {
      const out = decompress(buf);
      if (kindOf(out) !== "data") {
        return { data: out, compressed: true };
      }
    } catch {
      // not actually compressed
    }
  }
  return { data: buf, compressed: false };
}

export function kindOf(buf: Buffer): NodeKind {
  if (isArc(buf)) {
    return "arc";
  }
  if (isU8(buf)) {
    return "u8";
  }
  if (isBrres(buf)) {
    return "brres";
  }
  if (isRfnt(buf)) {
    return "rfnt";
  }
  return "data";
}

/** Walks a file (and everything nested in it), yielding every container/leaf node. */
export function* walk(buf: Buffer, basePath = ""): Generator<ContainerNode> {
  const { data, compressed } = unwrap(buf);
  const kind = kindOf(data);
  yield { path: basePath, kind, data, compressed };
  const join = (p: string) => (basePath ? `${basePath}/${p}` : p);
  if (kind === "arc") {
    const arc = parseArc(data);
    for (const e of arc.entries) {
      if (e.data.length) {
        yield* walk(e.data, join(e.name));
      }
    }
  } else if (kind === "u8") {
    for (const f of parseU8(data)) {
      yield* walk(f.data, join(f.path));
    }
  }
}

export type FoundTexture = BrresTexture & { brresPath: string; path: string };

/** Lists every TEX0 in every BRRES nested anywhere in `buf`. */
export function findTextures(buf: Buffer): FoundTexture[] {
  const out: FoundTexture[] = [];
  for (const node of walk(buf)) {
    if (node.kind !== "brres") {
      continue;
    }
    for (const t of listBrresTextures(parseBrres(node.data))) {
      out.push({ ...t, brresPath: node.path, path: `${node.path ? node.path + "/" : ""}Textures(NW4R)/${t.tex.name}` });
    }
  }
  return out;
}

/** Lists every RFNT nested anywhere in `buf`. */
export function findFonts(buf: Buffer): { path: string; data: Buffer }[] {
  const out: { path: string; data: Buffer }[] = [];
  for (const node of walk(buf)) {
    if (node.kind === "rfnt") {
      out.push({ path: node.path, data: node.data });
    }
  }
  return out;
}
