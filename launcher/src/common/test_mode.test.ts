import { describe, expect, it } from "vitest";

import { readTestMode, TestModeError } from "./test_mode";

describe("readTestMode", () => {
  const env = {
    PPO_USER_DATA_DIR: " D:/scratch/a ",
    PPO_REMOTE_DEBUGGING_PORT: "9301",
    PPO_HARNESS_PORT: "50001",
    PPO_DOLPHIN_EXTRA_ARGS: '["-v", "D3D11"]',
  };

  it("is active unpackaged, or packaged with PPO_TEST_MODE=1", () => {
    expect(readTestMode(env, false)).toEqual({
      active: true,
      userDataDir: "D:/scratch/a",
      remoteDebuggingPort: 9301,
      harnessPort: 50001,
      dolphinExtraArgs: ["-v", "D3D11"],
      pplusRelease: null,
    });
    expect(readTestMode(env, true)).toMatchObject({
      active: false,
      userDataDir: null,
      remoteDebuggingPort: null,
      harnessPort: null,
      dolphinExtraArgs: [],
    });
    expect(readTestMode({ ...env, PPO_TEST_MODE: "1" }, true).active).toBe(true);
    expect(readTestMode({ ...env, PPO_TEST_MODE: "0" }, true).active).toBe(false);
  });

  it("defaults to nothing", () => {
    expect(readTestMode({}, false)).toEqual({
      active: true,
      userDataDir: null,
      remoteDebuggingPort: null,
      harnessPort: null,
      dolphinExtraArgs: [],
      pplusRelease: null,
    });
  });

  it("reads a replacement P+ release", () => {
    const release = { version: "test", url: "http://127.0.0.1:8123/p.zip", size: 1234, sha256: "ab".repeat(32) };
    expect(readTestMode({ PPO_PPLUS_RELEASE: JSON.stringify(release) }, false).pplusRelease).toEqual(release);
    expect(readTestMode({ PPO_PPLUS_RELEASE: JSON.stringify(release) }, true).pplusRelease).toBeNull();
    expect(() => readTestMode({ PPO_PPLUS_RELEASE: "{}" }, false)).toThrow(/PPO_PPLUS_RELEASE/);
    expect(() => readTestMode({ PPO_PPLUS_RELEASE: JSON.stringify({ ...release, size: -1 }) }, false)).toThrow(
      TestModeError,
    );
  });

  it("rejects malformed values", () => {
    expect(() => readTestMode({ PPO_HARNESS_PORT: "abc" }, false)).toThrow(TestModeError);
    expect(() => readTestMode({ PPO_REMOTE_DEBUGGING_PORT: "70000" }, false)).toThrow(TestModeError);
    expect(() => readTestMode({ PPO_DOLPHIN_EXTRA_ARGS: "-v D3D11" }, false)).toThrow(/JSON array/);
    expect(() => readTestMode({ PPO_DOLPHIN_EXTRA_ARGS: "[1]" }, false)).toThrow(/JSON array/);
    // ignored (not parsed) when inactive
    expect(readTestMode({ PPO_HARNESS_PORT: "abc" }, true).active).toBe(false);
  });
});
