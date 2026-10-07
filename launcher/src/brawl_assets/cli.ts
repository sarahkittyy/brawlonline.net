// Development CLI for the Brawl asset extraction.
//
//   npx ts-node --transpile-only src/brawl_assets/cli.ts --disc-folder <.../DATA/files> [--sd <sd.raw>] [--out <dir>]
//   npx ts-node --transpile-only src/brawl_assets/cli.ts --iso <game.iso> --dolphin-tool <DolphinTool.exe> ...
//   ... survey <disc|sd>:<relative path> [<regex>]    lists every texture / font in an archive
//
// SPDX-License-Identifier: GPL-3.0-or-later

import path from "path";

import type { DiscInput } from "./extract";
import { extractBrawlAssets } from "./extract";
import { findFonts, findTextures } from "./formats/container";
import { parseRfnt } from "./formats/rfnt";
import { DiscFolderSource, DiscIsoSource, SdSource } from "./sources";

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
  cli.ts (--disc-folder <dir> | --iso ...) [--sd ...] survey <disc|sd>:<path> [<regex>]`;

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
  console.log(`missing:  ${manifest.missing.join(", ") || "-"}`);
  if (manifest.warnings.length) {
    console.log(`warnings:\n  ${manifest.warnings.join("\n  ")}`);
  }
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
