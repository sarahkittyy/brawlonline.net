// Development CLI for the Brawl asset extraction.
//
//   npx ts-node --transpile-only src/brawl_assets/cli.ts --disc-folder <.../DATA/files> [--sd <sd.raw>] [--out <dir>]
//   npx ts-node --transpile-only src/brawl_assets/cli.ts --iso <game.iso> --dolphin-tool <DolphinTool.exe> ...
//   ... survey <disc|sd>:<relative path> [<regex>]    lists every texture / font in an archive
//   ... sounds [<regex>] [--details]                  lists the sound archive's sounds (id, name, type, file, group)
//   ... sound-export <name|id> <out.wav> [--origin disc|sd]   decodes one wave sound to a WAV file
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

import type { DiscInput } from "./extract";
import { extractBrawlAssets } from "./extract";
import { fileGroups, SOUND_TYPE_NAMES, SoundType } from "./formats/brsar";
import { findFonts, findTextures } from "./formats/container";
import { parseRfnt } from "./formats/rfnt";
import { WAVE_ENCODING_NAMES } from "./formats/rwsd";
import { SoundLibrary } from "./sounds";
import type { GameFileSource } from "./sources";
import { DiscFolderSource, DiscIsoSource, SdSource } from "./sources";
import { encodeWav } from "./wav";

type Args = { _: string[]; [flag: string]: string | boolean | string[] };

function parseArgs(argv: string[]): Args {
  const out: Args = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) {
      const key = a.slice(2);
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith("--")) {
        out[key] = next;
        i++;
      } else {
        out[key] = true;
      }
    } else {
      out._.push(a);
    }
  }
  return out;
}

const usage = `Usage:
  cli.ts (--disc-folder <dir> | --iso <image> --dolphin-tool <exe>) [--sd <sd.raw or folder>] [--out <dir>] [--all-costumes]
  cli.ts (--disc-folder <dir> | --iso ...) [--sd ...] survey <disc|sd>:<path> [<regex>]
  cli.ts (--disc-folder <dir> | --iso ...) [--sd ...] sounds [<regex>] [--details]
  cli.ts (--disc-folder <dir> | --iso ...) [--sd ...] sound-export <name|id> <out.wav> [--origin disc|sd]`;

function discInput(args: Args): DiscInput {
  if (typeof args["disc-folder"] === "string") {
    return { kind: "folder", path: args["disc-folder"] };
  }
  if (typeof args.iso === "string" && typeof args["dolphin-tool"] === "string") {
    return { kind: "iso", path: args.iso, dolphinToolPath: args["dolphin-tool"] };
  }
  throw new Error(usage);
}

async function survey(args: Args, spec: string, filter?: string) {
  const [origin, rel] = spec.split(/:(.+)/);
  const disc = discInput(args);
  const src =
    origin === "sd"
      ? await SdSource.open(String(args.sd))
      : disc.kind === "folder"
      ? await DiscFolderSource.open(disc.path)
      : await DiscIsoSource.open({ isoPath: disc.path, dolphinToolPath: disc.dolphinToolPath });
  try {
    const buf = await src.read(rel);
    if (!buf) {
      throw new Error(`${spec}: not found`);
    }
    const re = filter ? new RegExp(filter) : null;
    for (const t of findTextures(buf)) {
      if (!re || re.test(t.path)) {
        console.log(`tex  ${t.path}  ${t.tex.formatName} ${t.tex.width}x${t.tex.height}${t.palette ? " +PLT0" : ""}`);
      }
    }
    for (const f of findFonts(buf)) {
      const r = parseRfnt(f.data);
      console.log(
        `font ${f.path || "<root>"}  ${r.sheetFormatName} cell ${r.cellWidth}x${r.cellHeight} ` +
          `baseline ${r.baseline} sheets ${r.sheetCount}x${r.sheetWidth}x${r.sheetHeight} chars ${r.codeToGlyph.size}`,
      );
    }
  } finally {
    await src.close();
  }
}

async function openSources(args: Args): Promise<{ disc: GameFileSource; sd: GameFileSource | null }> {
  const d = discInput(args);
  const disc =
    d.kind === "folder"
      ? await DiscFolderSource.open(d.path)
      : await DiscIsoSource.open({ isoPath: d.path, dolphinToolPath: d.dolphinToolPath });
  const sd = typeof args.sd === "string" ? await SdSource.open(args.sd) : null;
  return { disc, sd };
}

async function withSounds(args: Args, fn: (lib: SoundLibrary) => Promise<void>) {
  const { disc, sd } = await openSources(args);
  const files = new Map<string, Promise<Buffer | null>>();
  const lib = new SoundLibrary((origin, file) => {
    const src = origin === "sd" ? sd : disc;
    if (!src) {
      return Promise.resolve(null);
    }
    const key = `${origin}:${file}`;
    if (!files.has(key)) {
      files.set(key, src.read(file));
    }
    return files.get(key)!;
  });
  try {
    await fn(lib);
  } finally {
    await sd?.close();
    await disc.close();
  }
}

async function listSounds(args: Args, filter?: string) {
  await withSounds(args, async (lib) => {
    const archive = await lib.archive("disc");
    if (!archive) {
      throw new Error("no sound archive on the disc");
    }
    const { brsar } = archive;
    const re = filter ? new RegExp(filter, "i") : null;
    for (const s of brsar.sounds) {
      if (re && !re.test(s.name)) {
        continue;
      }
      const groups = [...new Set(fileGroups(brsar, s.fileId).map((g) => `${g.id}:${g.name ?? "-"}`))].join(",");
      let line = `${s.id}\t${s.name}\t${SOUND_TYPE_NAMES[s.soundType] ?? s.soundType}\tfile ${s.fileId}\tentry ${
        s.index
      }\tgroup ${groups}`;
      if (args.details && s.soundType === SoundType.Wave) {
        try {
          const l = await lib.load({ origin: "disc", sound: s.name });
          if (l) {
            const w = l.decoded.wave;
            line += `\t${WAVE_ENCODING_NAMES[w.encoding]} ${w.channels.length}ch ${l.decoded.sampleRate} Hz ${
              l.durationMs
            } ms${w.looped ? " loop" : ""}`;
          }
        } catch (err: any) {
          line += `\t(${err?.message ?? err})`;
        }
      }
      console.log(line);
    }
  });
}

async function exportSound(args: Args, nameOrId: string, out: string) {
  if (!nameOrId || !out) {
    throw new Error(usage);
  }
  const origin = args.origin === "sd" ? "sd" : "disc";
  await withSounds(args, async (lib) => {
    const archive = await lib.archive("disc");
    const sound = /^\d+$/.test(nameOrId) ? archive?.brsar.sounds[Number(nameOrId)]?.name : nameOrId;
    const l = sound ? await lib.load({ origin, sound }) : null;
    if (!l) {
      throw new Error(`${nameOrId}: not found${origin === "sd" ? " (or not replaced on the SD card)" : ""}`);
    }
    fs.writeFileSync(out, encodeWav(l.decoded.sampleRate, l.decoded.channels));
    console.log(
      `${l.sound.id} ${l.sound.name} (${l.group.name}) from ${origin}:${l.source} -> ${out}: ` +
        `${l.decoded.channels.length}ch ${l.decoded.sampleRate} Hz ${l.durationMs} ms`,
    );
  });
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || args.h) {
    console.log(usage);
    return;
  }
  if (args._[0] === "survey") {
    await survey(args, args._[1], args._[2]);
    return;
  }
  if (args._[0] === "sounds") {
    await listSounds(args, args._[1]);
    return;
  }
  if (args._[0] === "sound-export") {
    await exportSound(args, args._[1], args._[2]);
    return;
  }
  const out = typeof args.out === "string" ? args.out : path.resolve(__dirname, "..", "..", ".asset-cache");
  const started = Date.now();
  let last = "";
  const manifest = await extractBrawlAssets({
    disc: discInput(args),
    sdRawPath: typeof args.sd === "string" ? args.sd : undefined,
    cacheDir: out,
    allCostumes: !!args["all-costumes"],
    onProgress: (done, total, label) => {
      const line = `[${done}/${total}] ${label}`;
      if (line !== last) {
        process.stdout.write(`${line}\n`);
        last = line;
      }
    },
  });
  const secs = ((Date.now() - started) / 1000).toFixed(1);
  console.log(`\nWrote ${out} in ${secs}s`);
  console.log(`textures: ${Object.keys(manifest.textures).join(", ") || "-"}`);
  console.log(`fonts:    ${Object.keys(manifest.fonts).join(", ") || "-"}`);
  console.log(`stocks:   ${Object.keys(manifest.stocks).length}`);
  console.log(
    `sounds:   ${
      Object.entries(manifest.sounds ?? {})
        .map(([k, v]) => `${k} (${v.sound}, ${v.durationMs} ms, ${v.origin})`)
        .join(", ") || "-"
    }`,
  );
  console.log(`missing:  ${manifest.missing.join(", ") || "-"}`);
  if (manifest.warnings.length) {
    console.log(`warnings:\n  ${manifest.warnings.join("\n  ")}`);
  }
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
