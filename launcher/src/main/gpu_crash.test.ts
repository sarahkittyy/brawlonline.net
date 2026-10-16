import { EventEmitter } from "events";
import { describe, expect, it, vi } from "vitest";

import { turnsOffHardwareAcceleration, watchGpuCrashes } from "./gpu_crash";

describe("turnsOffHardwareAcceleration", () => {
  it("is a GPU process crash on Linux", () => {
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "crashed" }, "linux")).toBe(true);
  });

  it("is not a GPU process that was killed or exited, another process, or another OS", () => {
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "killed" }, "linux")).toBe(false);
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "abnormal-exit" }, "linux")).toBe(false);
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "clean-exit" }, "linux")).toBe(false);
    expect(turnsOffHardwareAcceleration({ type: "Utility", reason: "crashed" }, "linux")).toBe(false);
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "crashed" }, "win32")).toBe(false);
    expect(turnsOffHardwareAcceleration({ type: "GPU", reason: "crashed" }, "darwin")).toBe(false);
  });
});

describe("watchGpuCrashes", () => {
  it("turns hardware acceleration off when the GPU process crashes, and logs it", () => {
    const app = new EventEmitter();
    const turnOff = vi.fn();
    const warn = vi.fn();
    watchGpuCrashes(app as any, turnOff, { warn }, "linux");

    app.emit("child-process-gone", {}, { type: "GPU", reason: "killed", exitCode: 9 });
    expect(turnOff).not.toHaveBeenCalled();

    app.emit("child-process-gone", {}, { type: "GPU", reason: "crashed", exitCode: 139 });
    expect(turnOff).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toContain("The GPU process crashed (exit code 139)");
  });
});
