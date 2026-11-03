import { describe, expect, it } from "vitest";

import { parsePadsLine } from "./pad_reader";

describe("DolphinTool pads lines", () => {
  it("reads buttons", () => {
    expect(parsePadsLine('{"buttons":352}')).toEqual({ buttons: 352 });
  });

  it("reads the status as DolphinTool prints it", () => {
    // The scratch build's first line with Sarah's netplay controller settings (no adapter plugged in).
    const line =
      '{"adapter":"not_detected","ports":[{"device":"GameCube adapter","kind":"adapter","plugged":false,"port":1},' +
      '{"device":"DInput\\/0\\/Arduino Leonardo","kind":"controller","plugged":false,"port":3}]}';
    expect(parsePadsLine(line)).toEqual({
      status: {
        adapter: "not_detected",
        ports: [
          { port: 1, kind: "adapter", device: "GameCube adapter", plugged: false },
          { port: 3, kind: "controller", device: "DInput/0/Arduino Leonardo", plugged: false },
        ],
      },
    });
  });

  it("keeps the adapter's error", () => {
    expect(parsePadsLine('{"adapter":"error","adapter_error":"Access denied","ports":[]}')).toEqual({
      status: { adapter: "error", adapterError: "Access denied", ports: [] },
    });
  });

  it("skips anything else", () => {
    expect(parsePadsLine("")).toBeNull();
    expect(parsePadsLine("usage: dolphin-tool COMMAND -h")).toBeNull();
    expect(parsePadsLine('{"buttons":1.5}')).toBeNull();
    expect(parsePadsLine("[1,2]")).toBeNull();
  });
});
