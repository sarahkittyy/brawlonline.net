// Development CLI: makes pplus_build_fingerprints.json, which tells a player's SD card's Project+ release by its
// /Project+/NETPLAY.GCT (pplus_builds.ts). For every release of P+'s GitHub repository it streams the
// Windows Netplay zip's user/Wii/sd.raw and stops once the card's FAT, NETPLAY.GCT and info.xml are
// read (the first few hundred MB of the 2 GB card), so nothing is downloaded whole.
//
//   npx ts-node --transpile-only src/dolphin/install/pplus_builds_cli.ts [--out <file>]
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { createHash } from "crypto";
import fs from "fs";
import path from "path";
import { Readable } from "stream";
import type { ReadableStream as WebReadableStream } from "stream/web";
import zlib from "zlib";

import { type ByteReader, FatImage } from "../../brawl_assets/fat32";
import { type PPlusBuild, PPLUS_INFO_PATH, PPLUS_PROBE_PATH, readInfoVersion } from "./pplus_builds";

const REPO = "Project-Plus-Development-Team/Project-Plus-Dolphin";

class NeedMore extends Error {}

/** The inflated card so far: reads past its end throw NeedMore. */
class GrowingReader implements ByteReader {
  private chunks: Buffer[] = [];
  private starts: number[] = [];
  length = 0;
  push(chunk: Buffer) {
    this.chunks.push(chunk);
    this.starts.push(this.length);
    this.length += chunk.length;
  }
  async read(position: number, length: number): Promise<Buffer> {
    if (position + length > this.length) {
      throw new NeedMore();
    }
    const out = Buffer.alloc(length);
    let i = this.starts.findIndex((s, k) => s + this.chunks[k].length > position);
    let done = 0;
    while (done < length) {
      const c = this.chunks[i];
      const from = position + done - this.starts[i];
      const n = Math.min(c.length - from, length - done);
      c.copy(out, done, from, from + n);
      done += n;
      i++;
    }
    return out;
  }
  async close() {
    this.chunks = [];
  }
}

async function range(url: string, from: number, to?: number): Promise<Response> {
  const res = await fetch(url, { headers: { Range: `bytes=${from}-${to ?? ""}` } });
  if (res.status !== 206) {
    throw new Error(`${url}: HTTP ${res.status} for a range request`);
  }
  return res;
}

const bytes = async (res: Response) => Buffer.from(await res.arrayBuffer());

/** The card's entry in the zip's central directory (zip64 aware). */
async function findCardEntry(url: string, size: number) {
  const tailLen = Math.min(size, 1024 * 1024);
  const tail = await bytes(await range(url, size - tailLen, size - 1));
  const eocd = tail.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  if (eocd < 0) {
    throw new Error("no end of central directory");
  }
  let cdSize = tail.readUInt32LE(eocd + 12);
  let cdOffset = tail.readUInt32LE(eocd + 16);
  if (cdOffset === 0xffffffff || cdSize === 0xffffffff) {
    const z64 = tail.lastIndexOf(Buffer.from([0x50, 0x4b, 0x06, 0x06]));
    cdSize = Number(tail.readBigUInt64LE(z64 + 40));
    cdOffset = Number(tail.readBigUInt64LE(z64 + 48));
  }
  const cd =
    cdOffset >= size - tailLen
      ? tail.subarray(cdOffset - (size - tailLen), cdOffset - (size - tailLen) + cdSize)
      : await bytes(await range(url, cdOffset, cdOffset + cdSize - 1));
  for (let p = 0; p + 46 <= cd.length && cd.readUInt32LE(p) === 0x02014b50; ) {
    const method = cd.readUInt16LE(p + 10);
    let compressed = cd.readUInt32LE(p + 20);
    let uncompressed = cd.readUInt32LE(p + 24);
    const nameLen = cd.readUInt16LE(p + 28);
    const extraLen = cd.readUInt16LE(p + 30);
    const commentLen = cd.readUInt16LE(p + 32);
    let local = cd.readUInt32LE(p + 42);
    const name = cd.toString("utf8", p + 46, p + 46 + nameLen);
    if (/(^|\/)user\/wii\/sd\.raw$/i.test(name)) {
      // zip64 extra field: the 0xffffffff fields, in this order
      let e = p + 46 + nameLen;
      const end = e + extraLen;
      while (e + 4 <= end) {
        const id = cd.readUInt16LE(e);
        const len = cd.readUInt16LE(e + 2);
        if (id === 1) {
          let q = e + 4;
          if (uncompressed === 0xffffffff) {
            uncompressed = Number(cd.readBigUInt64LE(q));
            q += 8;
          }
          if (compressed === 0xffffffff) {
            compressed = Number(cd.readBigUInt64LE(q));
            q += 8;
          }
          if (local === 0xffffffff) {
            local = Number(cd.readBigUInt64LE(q));
          }
        }
        e += 4 + len;
      }
      return { name, method, compressed, uncompressed, local };
    }
    p += 46 + nameLen + extraLen + commentLen;
  }
  throw new Error("no user/Wii/sd.raw in the zip");
}

/** Streams the card until NETPLAY.GCT and info.xml read; returns their fingerprint. */
async function fingerprint(url: string, size: number): Promise<{ netplayGct: string; infoVersion: string | null }> {
  const entry = await findCardEntry(url, size);
  const header = await bytes(await range(url, entry.local, entry.local + 29));
  const dataStart = entry.local + 30 + header.readUInt16LE(26) + header.readUInt16LE(28);
  const controller = new AbortController();
  const res = await fetch(url, {
    headers: { Range: `bytes=${dataStart}-${dataStart + entry.compressed - 1}` },
    signal: controller.signal,
  });
  if (res.status !== 206 || !res.body) {
    throw new Error(`HTTP ${res.status}`);
  }
  const reader = new GrowingReader();
  const inflate = entry.method === 8 ? zlib.createInflateRaw() : null;
  let tried = 0;
  const attempt = async () => {
    try {
      const image = await FatImage.fromReader(reader);
      const gct = await image.readFile(PPLUS_PROBE_PATH);
      const info = (await image.exists(PPLUS_INFO_PATH)) ? await image.readFile(PPLUS_INFO_PATH) : null;
      return {
        netplayGct: createHash("sha256").update(gct).digest("hex"),
        infoVersion: info ? readInfoVersion(info) : null,
      };
    } catch (err) {
      if (err instanceof NeedMore) {
        return null;
      }
      throw err;
    }
  };
  const body = Readable.fromWeb(res.body as WebReadableStream<Uint8Array>);
  const card: AsyncIterable<Buffer> = inflate ? body.pipe(inflate) : body;
  try {
    for await (const chunk of card) {
      reader.push(chunk);
      if (reader.length - tried >= 16 * 1024 * 1024) {
        tried = reader.length;
        const found = await attempt();
        if (found) {
          process.stderr.write(`  read ${Math.round(reader.length / 1048576)} MB of the card\n`);
          return found;
        }
      }
    }
    const found = await attempt();
    if (found) {
      return found;
    }
    throw new Error("the card ended before NETPLAY.GCT was read");
  } finally {
    controller.abort();
    body.destroy();
    inflate?.destroy();
  }
}

async function main() {
  const outIndex = process.argv.indexOf("--out");
  const out = outIndex > 0 ? process.argv[outIndex + 1] : path.join(__dirname, "pplus_build_fingerprints.json");
  const releases = (await (
    await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=100`, {
      headers: { Accept: "application/vnd.github+json" },
    })
  ).json()) as {
    tag_name: string;
    published_at: string;
    assets: { name: string; size: number; browser_download_url: string }[];
  }[];
  // Releases already in `out` are kept (a run that stopped goes on where it was); saved after each one.
  let builds: PPlusBuild[] = [];
  try {
    builds = JSON.parse(await fs.promises.readFile(out, "utf8"));
  } catch {
    // a new file
  }
  const save = async () => {
    builds.sort((a, b) => a.publishedAt.localeCompare(b.publishedAt));
    await fs.promises.writeFile(out, JSON.stringify(builds, null, 2) + "\n");
  };
  for (const r of releases) {
    if (builds.some((b) => b.release === r.tag_name)) {
      continue;
    }
    const asset =
      r.assets.find((a) => /Netplay\.Windows\.zip$/i.test(a.name)) ??
      r.assets.find((a) => /Windows\.zip$/i.test(a.name) && !/Update/i.test(a.name));
    if (!asset) {
      process.stderr.write(`${r.tag_name}: no Windows zip, skipped\n`);
      continue;
    }
    process.stderr.write(`${r.tag_name}: ${asset.name}\n`);
    try {
      const { netplayGct, infoVersion } = await fingerprint(asset.browser_download_url, asset.size);
      builds.push({ release: r.tag_name, publishedAt: r.published_at.slice(0, 10), netplayGct, infoVersion });
      await save();
      process.stderr.write(`  NETPLAY.GCT ${netplayGct}, info.xml ${infoVersion}\n`);
    } catch (err) {
      process.stderr.write(`  failed: ${err instanceof Error ? err.message : String(err)}\n`);
    }
  }
  await save();
  process.stderr.write(`wrote ${builds.length} releases to ${out}\n`);
}

void main().catch((err) => {
  console.error(err);
  process.exit(1);
});
