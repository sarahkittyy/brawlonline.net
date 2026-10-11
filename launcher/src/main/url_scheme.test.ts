import { describe, expect, it } from "vitest";

import { linuxDesktopEntry } from "./url_scheme";

describe("url scheme", () => {
  it("writes a desktop entry that opens links with the AppImage", () => {
    const entry = linuxDesktopEntry('/home/deck/Apps/Brawl "Online" $1.AppImage');
    // The desktop entry spec's quoting: `"`, `$`, backquote and backslash get a backslash.
    const exec = String.raw`Exec="/home/deck/Apps/Brawl \"Online\" \$1.AppImage" %u`;
    expect(entry).toContain(`${exec}\n`);
    expect(entry).toContain("MimeType=x-scheme-handler/brawlonline;\n");
    expect(entry).toContain("NoDisplay=true\n");
    expect(entry.startsWith("[Desktop Entry]\n")).toBe(true);
  });
});
