/**
 * Whether a Dolphin's version (LocalInstallation.getDolphinVersion: the semver in `--version`, else
 * its first line) is Brawl Online's build. Play and replays need it: other Dolphins boot the game
 * but never answer its online requests, so a search spins forever (bug report #15: Project+ 3.2's
 * Dolphin, "3.2.0").
 *
 * Our releases print "Project+ Dolphin v0.1.<CI run>"; Project+'s own builds are 2.x/3.x and
 * Slippi's 3.x. Our local builds print "Project+ Dolphin <commit>" (no semver), as do old Project+
 * builds, which therefore pass. An unknown version is not judged.
 */
export function isBrawlOnlineDolphinVersion(version: string | undefined | null): boolean {
  if (!version) {
    return true;
  }
  const semver = version.match(/(\d+)\.\d+\.\d+/);
  if (semver) {
    return semver[1] === "0";
  }
  return version.startsWith("Project+ Dolphin");
}
