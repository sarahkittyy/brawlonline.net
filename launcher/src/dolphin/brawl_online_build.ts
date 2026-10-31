/**
 * What LocalInstallation.getDolphinVersion gives for a Dolphin that ran `--version` fine but printed
 * nothing: Project+ 3.2's own Windows build does (the card "Brawl Online accepts the P+ netplay
 * build off the website without errors"). Ours always prints its version.
 */
export const SILENT_DOLPHIN_VERSION = "no version printed";

/**
 * Whether a Dolphin's version (LocalInstallation.getDolphinVersion: the semver in `--version`, else
 * its first line) is Brawl Online's build. Play and replays need it: other Dolphins boot the game
 * but never answer its online requests, so a search spins forever (bug report #15: Project+ 3.2's
 * Dolphin, "3.2.0").
 *
 * Our releases print "Project+ Dolphin v0.1.<CI run>"; Project+'s own builds are 2.x/3.x or print
 * nothing, and Slippi's 3.x. Our local builds print "Project+ Dolphin <commit>" (no semver), as do
 * old Project+ builds, which therefore pass. An unknown version (`--version` failed) is not judged.
 */
export function isBrawlOnlineDolphinVersion(version: string | undefined | null): boolean {
  if (!version) {
    return true;
  }
  if (version === SILENT_DOLPHIN_VERSION) {
    return false;
  }
  const semver = version.match(/(\d+)\.\d+\.\d+/);
  if (semver) {
    return semver[1] === "0";
  }
  return version.startsWith("Project+ Dolphin");
}

/** Why Play, a replay or a room does not start the Dolphin Settings name (`version` not ours). */
export function notBrawlOnlineDolphinMessage(executable: string, version: string): string {
  const shown = version === SILENT_DOLPHIN_VERSION ? "" : ` (version ${version})`;
  return (
    `The Dolphin set in Settings > Dolphin is not Brawl Online's: ${executable}${shown}. ` +
    "Other Dolphins, such as the one in Project+'s own download, show the online menus but cannot connect to anyone. " +
    'Press "Use the default" under it in Settings > Dolphin to play with Brawl Online\'s Dolphin.'
  );
}
