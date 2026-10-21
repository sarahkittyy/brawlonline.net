// Which Project+ release a replay was recorded on (docs/design/pplus-versions.md). A replay only
// plays as it was played on the same release's game files: v3.2.1 changed the codeset, fighters and
// stages, so a v3.2.0 game run on v3.2.1's card is refused or plays differently.
//
// Recordings carry it in INFO `build.pplus` (from the launcher's installed release). Older ones
// don't: their `build.dolphin` is the launcher build that recorded them, `v0.1.<CI run number>`,
// and the launcher moved from v3.2.0 to v3.2.1 between run 89 and run 90 (afda0da8, 5e41bf9e). A
// development build writes a commit hash there: its release is unknown.
//
// SPDX-License-Identifier: GPL-3.0-or-later

/** The last launcher build (CI run) that played Project+ v3.2.0. */
const LAST_V3_2_0_BUILD = 89;

const RELEASE_RE = /^v\d{1,3}\.\d{1,3}(\.\d{1,3})?([a-z][0-9a-z]{0,8})?$/;

export const isPPlusRelease = (s: unknown): s is string => typeof s === "string" && RELEASE_RE.test(s);

/** A replay's P+ release from its INFO `build`, or null when it is unknown (a development build). */
export function replayPPlusRelease(build: { pplus?: unknown; dolphin?: unknown }): string | null {
  if (isPPlusRelease(build.pplus)) {
    return build.pplus;
  }
  const m = typeof build.dolphin === "string" ? /^v0\.1\.(\d{1,6})$/.exec(build.dolphin) : null;
  if (!m) {
    return null;
  }
  // (Builds from 90 on write build.pplus once they have it; until then they played v3.2.1.)
  return Number(m[1]) <= LAST_V3_2_0_BUILD ? "v3.2.0" : "v3.2.1";
}

/** "v3.2.0" -> "3.2.0", as the launcher shows a release ("Project+ 3.2.0"). */
export const pplusReleaseLabel = (release: string): string => release.replace(/^v/, "");

/** Orders releases: negative when `a` is older than `b`. */
export function comparePPlusReleases(a: string, b: string): number {
  const parts = (r: string) =>
    r
      .replace(/^v/, "")
      .split(/[.]/)
      .map((x) => parseInt(x, 10) || 0);
  const pa = parts(a);
  const pb = parts(b);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) {
      return d;
    }
  }
  return a.localeCompare(b);
}
