// `npm start` is three process trees: the renderer's dev server (webpack serve), which starts the
// preload watcher (start:preload) and electronmon (start:main) as its own children. They only
// stopped with it when it exited by itself. When it was killed instead (a tool stopping a
// background `npm start`, a kill by port or by command line), the other two lived on with nobody to
// stop them: electronmon reopened the launcher on every change to the main process's files in that
// checkout, days later (three dev launchers from 2026-11-15 were still doing it on 2026-11-20).
// The dev server passes its PID as ERB_DEV_SERVER_PID; once that process is gone, `close` runs.
module.exports = function exitWithDevServer(close) {
  const pid = Number(process.env.ERB_DEV_SERVER_PID);
  if (!pid) {
    return;
  }
  const timer = setInterval(() => {
    try {
      process.kill(pid, 0);
    } catch (err) {
      if (err.code === "EPERM") {
        return; // it exists
      }
      clearInterval(timer);
      close();
    }
  }, 2000);
  timer.unref();
};
