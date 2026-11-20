// electronmon's own CLI (node_modules/electronmon/bin/cli.js), through its API so it can be closed
// when `npm start`'s dev server is gone (exit-with-dev-server.js): electronmon would otherwise wait
// for the next file change and open the launcher again.
const fs = require("fs");
const path = require("path");

const electronmon = require("electronmon");

const exitWithDevServer = require("./exit-with-dev-server");

const pkg = JSON.parse(fs.readFileSync(path.resolve("package.json"), "utf8"));

process.title = `${pkg.name} - electronmon`;

const running = electronmon({
  cwd: fs.realpathSync(path.resolve(".")),
  args: process.argv.slice(2),
  logLevel: process.env.ELECTRONMON_LOGLEVEL || "info",
  electronPath: require("electron"),
  patterns: Array.isArray(pkg.electronmon?.patterns) ? pkg.electronmon.patterns : [],
});

exitWithDevServer(() => {
  console.log("[electronmon] the dev server is gone: closing the app");
  setTimeout(() => process.exit(0), 5000).unref();
  const exit = () => process.exit(0);
  return running.then((mon) => mon.destroy()).then(exit, exit);
});
