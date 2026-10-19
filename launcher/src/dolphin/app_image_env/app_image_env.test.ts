import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { sanitizeAppImageEnv } from "./app_image_env";

const ORIGINAL_PLATFORM = process.platform;

describe("sanitizeAppImageEnv", () => {
  beforeEach(() => {
    Object.defineProperty(process, "platform", { value: "linux" });
  });

  afterEach(() => {
    Object.defineProperty(process, "platform", { value: ORIGINAL_PLATFORM });
    vi.unstubAllEnvs();
  });

  it("returns process.env unchanged on non-Linux platforms", () => {
    Object.defineProperty(process, "platform", { value: "darwin" });
    const env = sanitizeAppImageEnv();
    expect(env).toBe(process.env);
  });

  it("removes AppImage-specific single-value env vars", () => {
    vi.stubEnv("APPDIR", "/tmp/.mount_SlippiXXX");
    vi.stubEnv("APPIMAGE", "/tmp/.mount_SlippiXXX/SlippiLauncher.AppImage");
    vi.stubEnv("ARGV0", "/tmp/.mount_SlippiXXX/SlippiLauncher.AppImage");
    vi.stubEnv("OWD", "/tmp");
    vi.stubEnv("PATH", "/usr/bin");
    const env = sanitizeAppImageEnv();
    expect(env).not.toHaveProperty("APPDIR");
    expect(env).not.toHaveProperty("APPIMAGE");
    expect(env).not.toHaveProperty("ARGV0");
    expect(env).not.toHaveProperty("OWD");
    expect(env).toHaveProperty("PATH", "/usr/bin");
  });

  it("filters AppImage mount path segments from LD_LIBRARY_PATH", () => {
    const appDir = "/tmp/.mount_SlippiXXX";
    vi.stubEnv("APPDIR", appDir);
    vi.stubEnv("LD_LIBRARY_PATH", `${appDir}/usr/lib:/usr/local/lib:/usr/lib`);
    const env = sanitizeAppImageEnv();
    expect(env.LD_LIBRARY_PATH).toEqual("/usr/local/lib:/usr/lib");
  });

  it("filters AppImage mount path segments from QT_PLUGIN_PATH", () => {
    const appDir = "/tmp/.mount_SlippiXXX";
    vi.stubEnv("APPDIR", appDir);
    vi.stubEnv("QT_PLUGIN_PATH", `${appDir}/usr/plugins:/usr/lib/qt/plugins`);
    const env = sanitizeAppImageEnv();
    expect(env.QT_PLUGIN_PATH).toEqual("/usr/lib/qt/plugins");
  });

  it("filters paths matching /tmp/.mount_ pattern even without APPDIR", () => {
    vi.stubEnv("LD_LIBRARY_PATH", "/tmp/.mount_OtherApp/usr/lib:/usr/lib");
    const env = sanitizeAppImageEnv();
    expect(env.LD_LIBRARY_PATH).toEqual("/usr/lib");
  });

  it("preserves legitimate path entries", () => {
    vi.stubEnv("APPDIR", "/tmp/.mount_SlippiXXX");
    vi.stubEnv("LD_LIBRARY_PATH", "/usr/lib:/usr/local/lib:/home/user/lib");
    const env = sanitizeAppImageEnv();
    expect(env.LD_LIBRARY_PATH).toEqual("/usr/lib:/usr/local/lib:/home/user/lib");
  });

  it("handles all path-like env vars", () => {
    const appDir = "/tmp/.mount_SlippiXXX";
    vi.stubEnv("APPDIR", appDir);
    vi.stubEnv("PATH", `/usr/bin:${appDir}/usr/bin`);
    vi.stubEnv("LD_LIBRARY_PATH", `${appDir}/usr/lib:/usr/lib`);
    vi.stubEnv("QT_PLUGIN_PATH", `${appDir}/usr/plugins:/usr/lib/qt/plugins`);
    vi.stubEnv("QT_QPA_PLATFORM_PLUGIN_PATH", `${appDir}/usr/plugins/platforms`);
    vi.stubEnv("QML2_IMPORT_PATH", `${appDir}/usr/qml`);
    vi.stubEnv("XDG_DATA_DIRS", `${appDir}/usr/share:/usr/share`);
    const env = sanitizeAppImageEnv();
    expect(env.PATH).toEqual("/usr/bin");
    expect(env.LD_LIBRARY_PATH).toEqual("/usr/lib");
    expect(env.QT_PLUGIN_PATH).toEqual("/usr/lib/qt/plugins");
    expect(env.QT_QPA_PLATFORM_PLUGIN_PATH).toBeUndefined();
    expect(env.QML2_IMPORT_PATH).toBeUndefined();
    expect(env.XDG_DATA_DIRS).toEqual("/usr/share");
  });

  it("handles missing APPDIR gracefully", () => {
    vi.stubEnv("LD_LIBRARY_PATH", "/usr/lib");
    const env = sanitizeAppImageEnv();
    expect(env.LD_LIBRARY_PATH).toEqual("/usr/lib");
  });

  it("removes path var entirely when all segments are filtered out", () => {
    const appDir = "/tmp/.mount_SlippiXXX";
    vi.stubEnv("APPDIR", appDir);
    vi.stubEnv("QT_PLUGIN_PATH", `${appDir}/usr/plugins`);
    const env = sanitizeAppImageEnv();
    expect(env).not.toHaveProperty("QT_PLUGIN_PATH");
  });

  it("gives Dolphin back the LD_PRELOAD Steam started the launcher with", () => {
    const steam =
      "/home/deck/.local/share/Steam/ubuntu12_32/gameoverlayrenderer.so:/home/deck/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so";
    vi.stubEnv("PPO_STEAM_LD_PRELOAD", steam);
    vi.stubEnv("LD_PRELOAD", "");
    const env = sanitizeAppImageEnv();
    expect(env.LD_PRELOAD).toEqual(steam);
    expect(env).not.toHaveProperty("PPO_STEAM_LD_PRELOAD");
  });

  it("restores the whole saved LD_PRELOAD, other preloads included", () => {
    vi.stubEnv("PPO_STEAM_LD_PRELOAD", "/usr/lib/libfoo.so:/s/ubuntu12_64/gameoverlayrenderer.so");
    vi.stubEnv("LD_PRELOAD", "/usr/lib/libfoo.so");
    const env = sanitizeAppImageEnv();
    expect(env.LD_PRELOAD).toEqual("/usr/lib/libfoo.so:/s/ubuntu12_64/gameoverlayrenderer.so");
  });

  it("leaves LD_PRELOAD alone when the launcher was not started by Steam", () => {
    vi.stubEnv("PPO_STEAM_LD_PRELOAD", "");
    vi.stubEnv("LD_PRELOAD", "/usr/lib/libfoo.so");
    const env = sanitizeAppImageEnv();
    expect(env.LD_PRELOAD).toEqual("/usr/lib/libfoo.so");
    expect(env).not.toHaveProperty("PPO_STEAM_LD_PRELOAD");
  });
});
