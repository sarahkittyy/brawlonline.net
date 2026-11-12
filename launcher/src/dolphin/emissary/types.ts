// EmissaryRecomp (emissary.ts): Brawl as a native PC program, an add-on the launcher downloads,
// keeps up to date and plays Project+ with instead of Dolphin (docs/design/brawl-recompiled.md).

export const EMISSARY_REPO = "kalomaze/EmissaryRecomp";
export const EMISSARY_REPO_URL = `https://github.com/${EMISSARY_REPO}`;
export const EMISSARY_AUTHOR = "kalomaze";

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
};

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
    /** It has Brawl Online's online play (the read-only card and the online control); Play uses Dolphin otherwise. */
    playsOnline: boolean;
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
