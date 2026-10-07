// Stages our game plugin for packaging: copies PPOnline.rel into release/plugins/ and writes
// PPOnline.json ({file, size, sha256}) next to it. electron-builder ships that folder as
// <resources>/plugins (extraResources), and the packaged launcher checks the binary against the
// manifest before installing it on the SD card. See PPLUS_PORTING.md, "Shipping the plugin".
//
//   npm run stage:plugin                       # from ../game-code/PPOnline/PPOnline.rel
//   PPO_PLUGIN_PATH=path/to/PPOnline.rel npm run stage:plugin
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "../..");
const src = process.env.PPO_PLUGIN_PATH || path.resolve(root, "..", "game-code", "PPOnline", "PPOnline.rel");
const outDir = path.join(root, "release", "plugins");

if (!fs.existsSync(src)) {
  console.error(`stage-plugin: ${src} not found. Build game-code/PPOnline first or set PPO_PLUGIN_PATH.`);
  process.exit(1);
}
const data = fs.readFileSync(src);
const manifest = {
  file: "PPOnline.rel",
  size: data.length,
  sha256: crypto.createHash("sha256").update(data).digest("hex"),
};
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, "PPOnline.rel"), data);
fs.writeFileSync(path.join(outDir, "PPOnline.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(`stage-plugin: ${src} -> ${outDir} (${manifest.size} bytes, sha256 ${manifest.sha256})`);
