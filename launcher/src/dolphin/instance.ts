import type { ChildProcess } from "child_process";
import { execFile, spawn } from "child_process";
import { delay } from "common/delay";
import { randomBytes } from "crypto";
import { app } from "electron";
import electronLog from "electron-log";
import { EventEmitter } from "events";
import debounce from "lodash/debounce";
import { mkdirSync } from "node:fs";
import { readFile, unlink, writeFile } from "node:fs/promises";
import path from "path";
import { fileExists } from "utils/file_exists";

import { sanitizeAppImageEnv } from "./app_image_env/app_image_env";
import type { ReplayCommunication, ReplayPlaybackStatus } from "./types";

const log = electronLog.scope("dolphin/instance");
const isMac = process.platform === "darwin";

export class MacOsRosettaRequiredError extends Error {
  constructor() {
    super(
      `Rosetta is required to run this executable. Open the Terminal app and run "softwareupdate --install-rosetta".`,
    );
  }
}

const generateTempCommunicationFile = (): string => {
  const tmpDir = path.join(app.getPath("userData"), "temp");
  mkdirSync(tmpDir, { recursive: true });
  const uniqueId = randomBytes(12).toString("hex");
  const commFileName = `playback-comms-${uniqueId}.json`;
  const commFileFullPath = path.join(tmpDir, commFileName);
  return commFileFullPath;
};

export class DolphinInstance extends EventEmitter {
  protected process: ChildProcess | null = null;
  private executablePath: string;
  private baseParams: string[];

  /**
   * @param baseParams always passed first: the User folder (`-u`) and, for Play, the boot target (`-e`).
   * Unlike Slippi we do not pass `-b`: P+'s launcher DOL hands over to the disc, and batch mode
   * must not treat that hand-over as the end of emulation.
   */
  constructor(execPath: string, baseParams: string[] = []) {
    super();
    this.baseParams = [...baseParams];
    this.executablePath = execPath;
  }

  /***
   * Spawns the Dolphin instance with any additional command line parameters
   */
  async start(additionalParams?: string[]): Promise<void> {
    const params: string[] = [...this.baseParams];

    // Add additional params if necessary
    if (additionalParams && additionalParams.length > 0) {
      params.push(...additionalParams);
    }

    this.process = await this.startProcess(this.executablePath, params);

    this.process.on("close", (code, signal) => {
      this.emit("close", code, signal);
    });
    this.process.on("error", (err) => {
      this.emit("error", err);
    });

    let combinedString = "";

    const debouncedErrorLog = debounce((msg: string) => {
      combinedString = "";
      log.error(`Received Dolphin stderr message: ${msg}`);
    }, 500);

    this.process.stderr?.on("data", (data) => {
      combinedString += data.toString();
      debouncedErrorLog(combinedString);
    });
  }

  /** Ends the Dolphin process (a spectated stream's end, docs/design/spectating.md §6.2). */
  stop() {
    this.process?.kill();
  }

  private async startProcess(executablePath: string, params: string[]): Promise<ChildProcess> {
    return new Promise((resolve, reject) => {
      let started = false;
      let child: ChildProcess;
      // On macOS, the default process.spawn seems to have odd overhead and causes deadlocks in Dolphin's rendering
      // process - as best I can tell, it's not separating the event loops and just choking immediately.
      //
      // If you read the Node.js source, execFile basically goes through `.spawn` as well, but sets a few
      // different options along the way. Notably, there's a number of option flags that aren't available on the
      // SpawnOptions TypeScript hint, for whatever reason... so trying to pass them here blows up.
      //
      // tl;dr: for macOS, pass through execFile and set a massively high buffer for performance reasons. The returned
      // child process is ultimately the same, or close enough for now, and keeps the rest of the codebase intact.
      try {
        if (isMac) {
          child = execFile(executablePath, params, {
            // 100MB
            maxBuffer: 1000 * 1000 * 100,
          });
        } else {
          child = spawn(executablePath, params, {
            env: sanitizeAppImageEnv(),
          });
        }

        child.once("spawn", () => {
          started = true;
          resolve(child);
        });

        child.once("error", (err) => {
          if (!started) {
            reject(err);
          }
        });
      } catch (err: unknown) {
        if (isMac && process.arch === "arm64" && (err as NodeJS.ErrnoException).errno === -86) {
          // EBADARCH – bad CPU type in executable
          reject(new MacOsRosettaRequiredError());
          return;
        }
        reject(err);
      }
    });
  }
}

/** How often the playback Dolphin's status file is read (it writes it every 200 ms at most). */
const STATUS_POLL_MS = 250;

export class PlaybackDolphinInstance extends DolphinInstance {
  /** Closed to start again (another Project+ release's card): its CLOSED event says so. */
  replaced = false;
  private commPath: string;
  private lastWriteMs: number;
  private lastOptions: ReplayCommunication | null = null;
  private statusTimer: ReturnType<typeof setInterval> | null = null;
  private askedFor = new Set<string>();
  /**
   * Called when Dolphin refused the current replay because it was recorded with another version
   * (or its match could not be rebuilt here): the launcher asks the player, and `playAnyway()`
   * plays it all the same (docs/design/replays.md §6.5).
   */
  onNeedsPlayAnyway: ((status: ReplayPlaybackStatus) => void) | null = null;
  /** Every change of Dolphin's status (highlights show their progress from it). */
  onStatus: ((status: ReplayPlaybackStatus) => void) | null = null;
  private lastStatusText = "";

  constructor(execPath: string, baseParams: string[] = []) {
    super(execPath, baseParams);
    this.commPath = generateTempCommunicationFile();
    this.lastWriteMs = 0;

    // Delete the comm file (and Dolphin's status next to it) once Dolphin is closed
    this.on("close", async () => {
      if (this.statusTimer) {
        clearInterval(this.statusTimer);
        this.statusTimer = null;
      }
      for (const file of [this.commPath, `${this.commPath}.status.json`]) {
        try {
          if (await fileExists(file)) {
            await unlink(file);
          }
        } catch (err) {
          log.warn(err);
        }
      }
    });
  }

  /** Dolphin's status of what it plays (`<comm>.status.json`), or null before it wrote one. */
  async readStatus(): Promise<ReplayPlaybackStatus | null> {
    try {
      const text = await readFile(`${this.commPath}.status.json`, "utf8");
      const status = JSON.parse(text);
      if (status === null || typeof status !== "object") {
        return null;
      }
      if (text !== this.lastStatusText) {
        this.lastStatusText = text;
        this.onStatus?.(status as ReplayPlaybackStatus);
      }
      return status as ReplayPlaybackStatus;
    } catch {
      return null;
    }
  }

  private watchStatus() {
    if (this.statusTimer) {
      return;
    }
    this.statusTimer = setInterval(() => {
      void this.readStatus().then((status) => {
        const id = this.lastOptions?.commandId;
        if (!status || !id || status.commandId !== id || !status.needsPlayAnyway || this.askedFor.has(id)) {
          return;
        }
        this.askedFor.add(id);
        this.onNeedsPlayAnyway?.(status);
      });
    }, STATUS_POLL_MS);
  }

  /** Plays the last request again, as one recorded with another version. */
  async playAnyway() {
    if (this.lastOptions) {
      // A new request (Dolphin acts on a new commandId) that still starts with the one it replaces.
      const id = this.lastOptions.commandId;
      await this.play({ ...this.lastOptions, commandId: id ? `${id}+anyway` : undefined, playAnyway: true });
    }
  }

  private async writeCommsFile(options: ReplayCommunication) {
    // Ours reads the whole file every 200 ms and acts on a new commandId (ReplayPlayback.cpp), so
    // highlights (the next row, at once) need no wait.
    if (options.mode === "highlights" || options.mode === "render") {
      await writeFile(this.commPath, JSON.stringify(options));
      this.lastWriteMs = Date.now();
      return;
    }
    // Playback Dolphin can't pick up comms file changes within the same second, so
    // defer writing comms file if it's been less than 1 second since last write.
    //
    // However this will only work if only one invocation comes 'early'.
    // If 2 or more invocations happen within a second after the last,
    // they will all be deferred to approximately the same time.
    // I don't think it's deterministic which one will go first (and therefore succeed).
    const diff = Date.now() - this.lastWriteMs;
    if (diff < 1000) {
      await delay(1000 - diff);
    }
    await writeFile(this.commPath, JSON.stringify(options));
    this.lastWriteMs = Date.now();
  }

  /***
   * Starts Dolphin with the specified replay communication options
   */
  async play(options: ReplayCommunication) {
    // Automatically generate a command id if not provided
    if (!options.commandId) {
      options.commandId = Math.random().toString(36).slice(2);
    }

    await this.writeCommsFile(options);
    this.lastOptions = { ...options };
    this.watchStatus();

    if (!this.process) {
      const params: string[] = [];
      // Launch this comms file
      params.push("-i", this.commPath);

      await this.start(params);
    }
  }
}
