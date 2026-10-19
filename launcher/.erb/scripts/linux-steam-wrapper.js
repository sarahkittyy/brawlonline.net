/* eslint-disable @typescript-eslint/no-var-requires */
// afterPack hook, Linux only: puts a small sh wrapper where the Electron binary was.
//
// Steam starts a non-Steam game with its overlay in LD_PRELOAD
// (ubuntu12_32/ and ubuntu12_64/gameoverlayrenderer.so). In the AppImage, where
// chrome-sandbox can't be setuid, Chromium uses the user-namespace sandbox, and with
// the overlay preloaded its zygote never sends the boot message: Electron aborts with
// "FATAL:zygote_host_impl_linux.cc Check failed: . : Invalid argument (22)" before any
// launcher code runs (bug report #47). The wrapper takes only the overlay out of
// LD_PRELOAD, keeps the sandbox on, and saves the original value in
// PPO_STEAM_LD_PRELOAD, which sanitizeAppImageEnv gives back to Dolphin.
const fs = require("fs");
const path = require("path");

const REAL_SUFFIX = ".bin";

function wrapperScript(realName) {
  return `#!/bin/sh
# Brawl Online: start the launcher without Steam's overlay preloaded (see
# .erb/scripts/linux-steam-wrapper.js); Dolphin gets it back.
case \${LD_PRELOAD-} in
*gameoverlayrenderer.so*)
  ppo_kept=
  ppo_ifs=$IFS
  IFS=': '
  set -f
  for ppo_lib in $LD_PRELOAD; do
    case $ppo_lib in
    '' | *gameoverlayrenderer.so) ;;
    *) ppo_kept=\${ppo_kept:+$ppo_kept:}$ppo_lib ;;
    esac
  done
  set +f
  IFS=$ppo_ifs
  PPO_STEAM_LD_PRELOAD=$LD_PRELOAD
  export PPO_STEAM_LD_PRELOAD
  if [ -n "$ppo_kept" ]; then
    LD_PRELOAD=$ppo_kept
    export LD_PRELOAD
  else
    unset LD_PRELOAD
  fi
  unset ppo_kept ppo_ifs ppo_lib
  ;;
esac
case $0 in
*/*) ppo_dir=\${0%/*} ;;
*) ppo_dir=. ;;
esac
exec "$ppo_dir/${realName}" "$@"
`;
}

module.exports = async function (context) {
  if (context.electronPlatformName !== "linux") {
    return;
  }
  const exeName = context.packager.executableName;
  const exePath = path.join(context.appOutDir, exeName);
  const realName = exeName + REAL_SUFFIX;
  fs.renameSync(exePath, path.join(context.appOutDir, realName));
  fs.writeFileSync(exePath, wrapperScript(realName), { mode: 0o755 });
  fs.chmodSync(exePath, 0o755);
  console.log(`afterPack: ${exeName} is now a wrapper for ${realName} (Steam overlay preload)`);
};

module.exports.wrapperScript = wrapperScript;
