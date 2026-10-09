import { readFileSync } from "fs";
import path from "path";
import { describe, expect, it } from "vitest";

import { APP_ID, BASE_DOMAIN, defaultServiceUrls, LEGACY_PRODUCT_NAMES, PRODUCT_NAME, PRODUCT_SLUG } from "./product";

const root = path.resolve(__dirname, "..", "..");
const readJson = (rel: string) => JSON.parse(readFileSync(path.join(root, rel), "utf8"));

describe("product identity", () => {
  it("is set in one place: package metadata must match PRODUCT_NAME", () => {
    expect(readJson("package.json").productName).toBe(PRODUCT_NAME);
    expect(readJson("release/app/package.json").productName).toBe(PRODUCT_NAME);
    expect(readJson("electron-builder.json").productName).toBe(PRODUCT_NAME);
  });

  it("derives a filesystem- and URL-safe slug", () => {
    expect(PRODUCT_SLUG).toBe("brawl-online");
  });

  it("uses the reverse-DNS form of the product domain as the app id", () => {
    expect(BASE_DOMAIN.split(".").reverse().join(".")).toBe("net.brawlonline");
    expect(APP_ID).toBe("net.brawlonline.launcher");
    expect(readJson("electron-builder.json").appId).toBe(APP_ID);
  });

  it("does not list the current name as a legacy name", () => {
    expect(LEGACY_PRODUCT_NAMES).not.toContain(PRODUCT_NAME);
  });

  it("publishes launcher updates to the configured feed", () => {
    expect(readJson("electron-builder.json").publish.url).toBe(defaultServiceUrls.launcherUpdates);
  });

  it("defaults every host to the brawlonline.net domain", () => {
    expect(defaultServiceUrls.accountsApi).toBe("https://brawlonline.net");
    expect(defaultServiceUrls.website).toBe("https://brawlonline.net");
    expect(defaultServiceUrls.matchmakingHost).toBe("mm.brawlonline.net");
    expect(defaultServiceUrls.launcherUpdates).toBe("https://brawlonline.net/updates/launcher");
  });
});
