// Development/test switches read from the environment. They only apply to unpackaged runs
// (`npm start`, or Electron on a production build in release/app) or when PPO_TEST_MODE=1,
// so an installed launcher ignores them unless a test asks for them explicitly.
//
//   PPO_USER_DATA_DIR          Electron userData folder (settings, sessions, logs, Dolphin User folders)
//   PPO_REMOTE_DEBUGGING_PORT  Chromium remote-debugging port (DevTools protocol)
//   PPO_HARNESS_PORT           appended to the netplay Dolphin as `--harness-port <port>`
//   PPO_DOLPHIN_EXTRA_ARGS     JSON array of extra netplay Dolphin arguments
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type TestModeOptions = {
  /** True when the switches below apply. */
  active: boolean;
  userDataDir: string | null;
  remoteDebuggingPort: number | null;
  harnessPort: number | null;
  dolphinExtraArgs: string[];
};

export const INACTIVE_TEST_MODE: TestModeOptions = {
  active: false,
  userDataDir: null,
  remoteDebuggingPort: null,
  harnessPort: null,
  dolphinExtraArgs: [],
};

export class TestModeError extends Error {}

function value(env: Record<string, string | undefined>, name: string): string | null {
  const v = env[name];
  return v !== undefined && v.trim() !== "" ? v.trim() : null;
}

function port(env: Record<string, string | undefined>, name: string): number | null {
  const v = value(env, name);
  if (v === null) {
    return null;
  }
  const n = Number(v);
  if (!/^\d+$/.test(v) || !Number.isInteger(n) || n < 1 || n > 65535) {
    throw new TestModeError(`${name} must be a port number (1-65535), got ${JSON.stringify(v)}`);
  }
  return n;
}

/** Reads the test switches. Throws TestModeError on malformed values (only when active). */
export function readTestMode(env: Record<string, string | undefined>, isPackaged: boolean): TestModeOptions {
  const active = !isPackaged || value(env, "PPO_TEST_MODE") === "1";
  if (!active) {
    return INACTIVE_TEST_MODE;
  }
  let dolphinExtraArgs: string[] = [];
  const extra = value(env, "PPO_DOLPHIN_EXTRA_ARGS");
  if (extra !== null) {
    let parsed: unknown;
    try {
      parsed = JSON.parse(extra);
    } catch {
      parsed = null;
    }
    if (!Array.isArray(parsed) || !parsed.every((a) => typeof a === "string")) {
      throw new TestModeError(`PPO_DOLPHIN_EXTRA_ARGS must be a JSON array of strings, got ${extra}`);
    }
    dolphinExtraArgs = parsed;
  }
  return {
    active,
    userDataDir: value(env, "PPO_USER_DATA_DIR"),
    remoteDebuggingPort: port(env, "PPO_REMOTE_DEBUGGING_PORT"),
    harnessPort: port(env, "PPO_HARNESS_PORT"),
    dolphinExtraArgs,
  };
}
