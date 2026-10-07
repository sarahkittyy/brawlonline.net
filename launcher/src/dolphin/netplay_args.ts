import type { TestModeOptions } from "@common/test_mode";
import path from "path";

export type NetplayArgsInput = {
  /** Arguments selecting the User folder (`-u <User>`). */
  userArgs: string[];
  /** The launcher-managed SD card image with our plugin, or null to keep Dolphin's own setting. */
  sdCardImage: string | null;
  /** What to boot (`-e`), or null to just open Dolphin. */
  bootFile: string | null;
  testMode: Pick<TestModeOptions, "active" | "harnessPort" | "dolphinExtraArgs">;
};

/**
 * The netplay Dolphin's command line on Play.
 *
 * The SD card is chosen with `-C System.Section.Key=Value`, which sets Dolphin's command-line
 * config layer for this run only (never saved to Dolphin.ini): `[General] WiiSDCardPath` is
 * `MAIN_WII_SD_CARD_IMAGE_PATH`. The card is also forced in (`[Core] WiiSDCard`) and folder sync
 * off (`[Core] WiiSDCardEnableFolderSync`), which would otherwise rebuild the image from a folder.
 * Test-mode switches (harness port, extra arguments) go last.
 */
export function buildNetplayDolphinArgs(input: NetplayArgsInput): string[] {
  const args = [...input.userArgs];
  if (input.sdCardImage) {
    const sd = path.resolve(input.sdCardImage);
    if (sd.includes("=")) {
      // Dolphin splits `-C` values at '=': such a path would be cut short.
      throw new Error(`The SD card path cannot contain "=": ${sd}`);
    }
    args.push(
      "-C",
      `Dolphin.General.WiiSDCardPath=${sd}`,
      "-C",
      "Dolphin.Core.WiiSDCard=True",
      "-C",
      "Dolphin.Core.WiiSDCardEnableFolderSync=False",
    );
  }
  if (input.bootFile) {
    args.push("-e", input.bootFile);
  }
  if (input.testMode.active) {
    if (input.testMode.harnessPort !== null) {
      args.push("--harness-port", String(input.testMode.harnessPort));
    }
    args.push(...input.testMode.dolphinExtraArgs);
  }
  return args;
}
