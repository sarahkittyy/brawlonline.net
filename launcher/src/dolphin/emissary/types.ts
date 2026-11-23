// EmissaryRecomp (emissary.ts): Brawl as a native PC program, an add-on the launcher downloads,
// keeps up to date and plays Project+ with instead of Dolphin (docs/design/brawl-recompiled.md).

export const EMISSARY_REPO = "kalomaze/EmissaryRecomp";
export const EMISSARY_REPO_URL = `https://github.com/${EMISSARY_REPO}`;
export const EMISSARY_AUTHOR = "kalomaze";

/**
 * What the launcher can play with: bumped when what a release needs from the launcher changes (the
 * launch set-up: BRAWL_ONLINE_CORE's user folder, the hand-off files, the card and codes). A release
 * whose manifest names another protocol is not played (Dolphin plays instead).
 */
export const EMISSARY_PROTOCOL = 1;

/**
 * A release's `manifest.json` asset (EmissaryRecomp's CI): its version, the Brawl Online core it
 * was built from (a launcher version or a commit) and the launcher protocol it speaks.
 */
export type EmissaryManifest = {
  version: string;
  brawlOnline: string;
  protocol: number;
};

/** A release's download for this computer, as GitHub's API lists it. */
export type EmissaryRelease = {
  /** The release's tag (`v0.6.0`). */
  version: string;
  assetName: string;
  url: string;
  /** The archive's size in bytes. */
  size: number;
  /** GitHub's sha256 of the archive, when it gives one. */
  sha256: string | null;
  /** Its manifest.json asset's URL, when it has one (releases before CI have none). */
  manifestUrl: string | null;
  /** That manifest, once read; null without one (or when it could not be read). */
  manifest: EmissaryManifest | null;
};

/**
 * Why Play used Dolphin although Settings > Game says EmissaryRecomp (manager._launchEmissary):
 * `status` the add-on is not ready (its state), `noOnline` the release can't play online with this
 * launcher, `updateFailed` the required update could not be downloaded (`message` says why).
 */
export type EmissaryFallback =
  | { reason: "status"; status: EmissaryStatus }
  | { reason: "noOnline" }
  | { reason: "updateFailed"; message: string };

export type EmissaryJob = {
  kind: "install" | "update" | "gameFiles";
  /** download: the archive; unpack: into versions/<v>; gameFiles: the disc's files from the ISO. */
  phase: "download" | "unpack" | "gameFiles";
  current: number;
  total: number;
};

/**
 * Where the add-on stands (the card's states, docs/design/brawl-recompiled.md §5). Computed from
 * `installed`, `gameFiles`, `job`, `staged`, `running`; the renderer only shows it.
 */
export type EmissaryStatus =
  | "unavailable" // no release for this platform (macOS for now)
  | "needsDisc" // no valid Brawl disc in Settings > Game
  | "notInstalled"
  | "downloading" // download or unpack (job)
  | "preparingFiles" // the disc's files (job)
  | "needsFiles" // the program is here, the disc's files are not (or are another disc's)
  | "ready"
  | "updateAvailable"
  | "running";

export type EmissaryState = {
  status: EmissaryStatus;
  /** `<netplay>/emissary-recomp`. */
  folder: string;
  /** This platform has releases (Windows, Linux). */
  supported: boolean;
  installed: {
    version: string;
    /** The program's folder size. */
    programBytes: number;
    installedAt: string;
    /** It has Brawl Online's online play (the read-only card and the core), for this launcher; Play uses Dolphin otherwise. */
    playsOnline: boolean;
    /** The Brawl Online core it was built from (its manifest), null when unknown. */
    brawlOnline: string | null;
  } | null;
  /** The disc's extracted files (how big), null when there are none. */
  gameFiles: { bytes: number } | null;
  /** The Settings disc is valid and its files are the extracted ones. */
  gameFilesCurrent: boolean;
  /** The Settings disc can be used (a valid NTSC-U Brawl image). */
  discReady: boolean;
  /** GitHub's latest release for this platform; null while unknown (not asked yet, or offline). */
  latest: EmissaryRelease | null;
  /** Why the latest release is unknown. */
  latestError: string | null;
  /** Installed and the latest release (or the latest is unknown). */
  upToDate: boolean;
  /** What the next Download / Update fetches. */
  downloadBytes: number;
  /** What the program and the disc's files take once installed (the archive unpacked + the disc). */
  diskBytes: number;
  /** A newer version is downloaded and waits for the game to close. */
  staged: string | null;
  job: EmissaryJob | null;
  /** Its game is open. */
  running: boolean;
  /** The last job's error. */
  error: string | null;
};

export interface EmissaryService {
  getState(refresh?: boolean): Promise<EmissaryState>;
  /** Download / update the program and prepare the disc's files; resolves when done (Cancel rejects). */
  install(): Promise<EmissaryState>;
  cancel(): Promise<void>;
  remove(): Promise<void>;
  onState(handle: (state: EmissaryState) => void): () => void;
}
