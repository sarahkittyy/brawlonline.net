// Stages our game plugin for packaging: copies its files into release/plugins/ and writes a
// manifest ({file, size, sha256}) next to each. The plugin is built once per kind of build
// (game-code/build.sh): PPOnline.rel for Project+, PPOnline-pex.rel for P+ v3.2 with BrawlEx
// (Project+ Ex, Cobalt), PPOnline-rex.rel for REX. electron-builder ships that folder as
// <resources>/plugins (extraResources), and the packaged launcher checks a binary against its
// manifest before installing it on the SD card. See PPLUS_PORTING.md, "Shipping the plugin".
//
//   npm run stage:plugin                       # from ../game-code/PPOnline/PPOnline*.rel
//   PPO_PLUGIN_PATH=path/to/PPOnline.rel npm run stage:plugin   # the others from beside it
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "../..");
const own = process.env.PPO_PLUGIN_PATH || path.resolve(root, "..", "game-code", "PPOnline", "PPOnline.rel");
const outDir = path.join(root, "release", "plugins");
// (launcher/src/dolphin/install/paths.ts pluginFileName)
const FILES = ["PPOnline.rel", "PPOnline-pex.rel", "PPOnline-rex.rel"];

fs.mkdirSync(outDir, { recursive: true });
for (const file of FILES) {
  const src = file === FILES[0] ? own : path.join(path.dirname(own), file);
  if (!fs.existsSync(src)) {
    console.error(`stage-plugin: ${src} not found. Build game-code (build.sh) first or set PPO_PLUGIN_PATH.`);
    process.exit(1);
  }
  const data = fs.readFileSync(src);
  const manifest = {
    file,
    size: data.length,
    sha256: crypto.createHash("sha256").update(data).digest("hex"),
  };
  fs.writeFileSync(path.join(outDir, file), data);
  fs.writeFileSync(path.join(outDir, file.replace(/\.rel$/, ".json")), JSON.stringify(manifest, null, 2) + "\n");
  console.log(`stage-plugin: ${src} -> ${outDir} (${manifest.size} bytes, sha256 ${manifest.sha256})`);
}
