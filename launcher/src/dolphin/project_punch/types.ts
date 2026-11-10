// Project Punch (project_punch.ts): what Home's lab button, its dialog and Settings > Training show.

export const PROJECT_PUNCH_REPO_URL = "https://github.com/WispSSBM/ProjectPunch";
export const PROJECT_PUNCH_AUTHOR = "WispSSBM";

export type ProjectPunchRelease = {
  /** The release's tag (`v1.3.1`). */
  version: string;
  assetName: string;
  url: string;
  /** The zip's size in bytes. */
  size: number;
  /** GitHub's sha256 of the zip, when it gives one. */
  sha256: string | null;
};

export type ProjectPunchFile = { path: string; sha256: string; size: number };

export type ProjectPunchJob = {
  kind: "install" | "update";
  phase: "download" | "card";
  current: number;
  total: number;
};

export type ProjectPunchState = {
  /** `<netplay>/project-punch`. */
  folder: string;
  /** Its SD card (`<folder>/sd.raw`). */
  cardPath: string;
  installed: {
    version: string;
    cardBytes: number;
    pplusRelease: string;
    bootsToTraining: boolean;
    installedAt: string;
  } | null;
  /** GitHub's latest release; null while unknown (not asked yet, or offline). */
  latest: ProjectPunchRelease | null;
  /** Why the latest release is unknown. */
  latestError: string | null;
  /** The next download makes the card (none yet, or one from another Project+ release). */
  needsCard: boolean;
  /** Installed, its card is current, and it is the latest release (or the latest is unknown). */
  upToDate: boolean;
  /** Bytes the next Download / Update needs on disk: the zip, plus the card when it is made. */
  downloadBytes: number;
  /** A download running (it goes on while the dialog is closed). */
  job: ProjectPunchJob | null;
  /** Its game is open. */
  running: boolean;
  /** The last download's error. */
  error: string | null;
};

export interface ProjectPunchService {
  getState(refresh?: boolean): Promise<ProjectPunchState>;
  /** Download or update; resolves when done (errors reject; Cancel rejects too). */
  install(): Promise<ProjectPunchState>;
  cancel(): Promise<void>;
  remove(): Promise<void>;
  play(): Promise<void>;
  onState(handle: (state: ProjectPunchState) => void): () => void;
}
