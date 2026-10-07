// Nintendo U8 archives (magic 0x55AA382D).
//
// Layout as implemented in BrawlLib (BrawlCrate, GPL-3.0): BrawlLib/SSBB/Types/U8.cs,
// BrawlLib/SSBB/ResourceNodes/Archives/U8Node.cs; cross-checked with
// https://wiki.tockdom.com/wiki/U8_(File_Format).
//
// SPDX-License-Identifier: GPL-3.0-or-later

export const U8_MAGIC = 0x55aa382d;

export type U8File = { path: string; data: Buffer };

export function isU8(buf: Uint8Array): boolean {
  return buf.length >= 0x20 && buf[0] === 0x55 && buf[1] === 0xaa && buf[2] === 0x38 && buf[3] === 0x2d;
}

/** Lists every file in the archive with its full slash-separated path (no leading slash). */
export function parseU8(buf: Buffer): U8File[] {
  if (!isU8(buf)) {
    throw new Error("Not a U8 archive");
  }
  const rootOff = buf.readUInt32BE(4);
  const total = buf.readUInt32BE(rootOff + 8);
  const strBase = rootOff + total * 12;
  const files: U8File[] = [];
  const nameAt = (off: number) => {
    const end = buf.indexOf(0, strBase + off);
    return buf.toString("latin1", strBase + off, end < 0 ? buf.length : end);
  };
  // Directory stack: [endIndex, path]
  const stack: { end: number; path: string }[] = [{ end: total, path: "" }];
  for (let i = 1; i < total; i++) {
    while (stack.length > 1 && i >= stack[stack.length - 1].end) {
      stack.pop();
    }
    const n = rootOff + i * 12;
    const type = buf[n];
    const nameOff = buf.readUIntBE(n + 1, 3);
    const a = buf.readUInt32BE(n + 4);
    const b = buf.readUInt32BE(n + 8);
    const parentPath = stack[stack.length - 1].path;
    const name = nameAt(nameOff);
    const path = parentPath ? `${parentPath}/${name}` : name;
    if (type === 1) {
      stack.push({ end: b, path });
    } else {
      if (a + b > buf.length) {
        throw new Error(`U8: file ${path} out of range`);
      }
      files.push({ path, data: buf.subarray(a, a + b) });
    }
  }
  return files;
}

/** Builds a U8 archive from flat paths (used by tests). Directories are created implicitly. */
export function buildU8(files: { path: string; data: Uint8Array }[]): Buffer {
  type Dir = { name: string; dirs: Map<string, Dir>; files: { name: string; data: Uint8Array }[] };
  const root: Dir = { name: "", dirs: new Map(), files: [] };
  for (const f of files) {
    const parts = f.path.split("/");
    let d = root;
    for (const p of parts.slice(0, -1)) {
      let next = d.dirs.get(p);
      if (!next) {
        next = { name: p, dirs: new Map(), files: [] };
        d.dirs.set(p, next);
      }
      d = next;
    }
    d.files.push({ name: parts[parts.length - 1], data: f.data });
  }
  type Node = { type: number; name: string; parent: number; end: number; data?: Uint8Array };
  const nodes: Node[] = [];
  const walk = (d: Dir, parent: number) => {
    const idx = nodes.length;
    const node: Node = { type: 1, name: d.name, parent, end: 0 };
    nodes.push(node);
    for (const f of d.files) {
      nodes.push({ type: 0, name: f.name, parent: idx, end: 0, data: f.data });
    }
    for (const sub of d.dirs.values()) {
      walk(sub, idx);
    }
    node.end = nodes.length;
  };
  walk(root, 0);
  const names: number[] = [];
  const strParts: Buffer[] = [];
  let strLen = 0;
  for (const n of nodes) {
    names.push(strLen);
    const s = Buffer.from(n.name + "\0", "latin1");
    strParts.push(s);
    strLen += s.length;
  }
  const rootOff = 0x20;
  const headerSize = nodes.length * 12 + strLen;
  const dataOff = Math.ceil((rootOff + headerSize) / 0x20) * 0x20;
  let cursor = dataOff;
  const offsets: number[] = [];
  for (const n of nodes) {
    offsets.push(cursor);
    if (n.type === 0) {
      cursor = Math.ceil((cursor + n.data!.length) / 0x20) * 0x20;
    }
  }
  const out = Buffer.alloc(cursor);
  out.writeUInt32BE(U8_MAGIC, 0);
  out.writeUInt32BE(rootOff, 4);
  out.writeUInt32BE(headerSize, 8);
  out.writeUInt32BE(dataOff, 12);
  nodes.forEach((n, i) => {
    const o = rootOff + i * 12;
    out[o] = n.type;
    out.writeUIntBE(names[i], o + 1, 3);
    if (n.type === 1) {
      out.writeUInt32BE(n.parent, o + 4);
      out.writeUInt32BE(n.end, o + 8);
    } else {
      out.writeUInt32BE(offsets[i], o + 4);
      out.writeUInt32BE(n.data!.length, o + 8);
      Buffer.from(n.data!).copy(out, offsets[i]);
    }
  });
  Buffer.concat(strParts).copy(out, rootOff + nodes.length * 12);
  return out;
}
