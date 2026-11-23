import { describe, expect, it } from "vitest";

import { operatingSystemName } from "./bootstrap";

describe("operatingSystemName", () => {
  it("names Windows 10 and 11 by the build number, without asking the system", () => {
    expect(operatingSystemName("win32", "10.0.19045")).toBe("Windows 10 (10.0.19045)");
    expect(operatingSystemName("win32", "10.0.22631")).toBe("Windows 11 (10.0.22631)");
    expect(operatingSystemName("win32", "10.0.26100")).toBe("Windows 11 (10.0.26100)");
    expect(operatingSystemName("win32", "6.3.9600")).toBe("Windows (6.3.9600)");
  });

  it("names macOS and Linux as before", () => {
    expect(operatingSystemName("darwin", "23.4.0")).toBe("macOS Sonoma (23.4.0)");
    expect(operatingSystemName("linux", "6.8.0-45-generic")).toBe("Linux 6.8 (6.8.0-45-generic)");
  });
});
