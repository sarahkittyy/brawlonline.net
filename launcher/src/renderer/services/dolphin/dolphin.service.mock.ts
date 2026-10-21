import type { ChatKeyPress } from "@common/chat_key";
import { chatKeyExpression, DEFAULT_CHAT_KEY } from "@common/chat_key";
import { delay } from "@common/delay";
import type { RoomHandOffResult } from "@dolphin/room_handoff";
import type {
  DolphinDownloadProgressEvent,
  DolphinEvent,
  DolphinEventMap,
  DolphinLaunchType,
  DolphinService,
  GameFilesMoveOutcome,
  PlayKey,
  PPlusPrepareResult,
  PPlusVersionInfo,
  ReplayPlayChoice,
  ReplayPlayResult,
  ReplayQueueItem,
} from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import type { HighlightPlayItem } from "@replays/highlights/types";
import { Observable, Subject } from "observable-fns";

import { delayAndMaybeError } from "../utils";

const SHOULD_ERROR = false;
const DOLPHIN_VERSION = "9.8.7";

class MockDolphinClient implements DolphinService {
  private eventSubject = new Subject<DolphinEvent>();
  private events = Observable.from(this.eventSubject);

  async getDolphinPaths(_dolphinType: DolphinLaunchType) {
    return {
      executable: "Dolphin.exe",
      userFolder: "User",
      playKeyFile: "User/Online/user.json",
      gameFilesFolder: "netplay",
    };
  }

  async moveGameFiles(folder: string | null): Promise<GameFilesMoveOutcome> {
    return { action: "moved", folder: folder ?? "netplay", leftBehind: null };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async downloadDolphin(dolphinType: DolphinLaunchType): Promise<void> {
    // Mock installation percentage
    for (let i = 0; i <= 100; i++) {
      const progressEvent: DolphinDownloadProgressEvent = {
        type: DolphinEventType.DOWNLOAD_PROGRESS,
        dolphinType,
        progress: {
          current: i,
          total: 100,
        },
      };
      await delay(50);
      this.eventSubject.next(progressEvent);
    }

    // Mark our download as complete
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_COMPLETE,
      dolphinType,
      dolphinVersion: DOLPHIN_VERSION,
    });
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async configureDolphin(_dolphinType: DolphinLaunchType): Promise<void> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async softResetDolphin(_dolphinType: DolphinLaunchType): Promise<void> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async hardResetDolphin(dolphinType: DolphinLaunchType): Promise<void> {
    await this.downloadDolphin(dolphinType);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async storePlayKeyFile(_key: PlayKey): Promise<void> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async checkPlayKeyExists(_key: PlayKey): Promise<boolean> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async removePlayKeyFile(): Promise<void> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async viewSlpReplay(_files: ReplayQueueItem[], _choice?: ReplayPlayChoice): Promise<ReplayPlayResult> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async playHighlights(
    _items: HighlightPlayItem[],
    _commandId: string,
    _choice?: ReplayPlayChoice,
  ): Promise<ReplayPlayResult> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async getPPlusVersions(): Promise<{ current: string; states: PPlusVersionInfo[] }> {
    const state = (release: string, current: boolean): PPlusVersionInfo => ({
      release,
      current,
      playable: true,
      filesReady: current,
      cardReady: false,
      downloadBytes: current ? 0 : 51_400_000,
      filesBytes: 0,
      cardBytes: 0,
    });
    return { current: "v3.2.1", states: [state("v3.2.0", false), state("v3.2.1", true)] };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async preparePPlusRelease(_release: string, _importFrom?: string): Promise<PPlusPrepareResult> {
    return { ok: false, code: "offline", message: "Not available in the mock." };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async removePPlusRelease(_release: string): Promise<void> {
    return;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async launchNetplayDolphin(_opts?: { bootGame?: boolean }): Promise<void> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async joinRoomInGame(_code: string, _ruleset?: string): Promise<RoomHandOffResult> {
    return { outcome: "not-running" };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async closeNetplayIfIdle(): Promise<boolean> {
    return true;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async prepareRoomLaunch(_code: string, _ruleset?: string): Promise<{ id: string }> {
    return { id: "mock-request" };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async cancelRoomLaunch(_id: string): Promise<void> {
    // Nothing was written.
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async openDolphinSettingsFolder(_dolphinType: DolphinLaunchType): Promise<void> {
    throw new Error("Method not implemented.");
  }

  private chatKey = DEFAULT_CHAT_KEY;

  async getChatKey(): Promise<string> {
    return this.chatKey;
  }

  async setChatKey(press: ChatKeyPress): Promise<string | null> {
    const chatKey = chatKeyExpression(press, "win32");
    if (chatKey !== null) {
      this.chatKey = chatKey;
    }
    return chatKey;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async installRosetta(): Promise<{ exitCode: number }> {
    return { exitCode: 0 };
  }

  onEvent<T extends DolphinEventType>(eventType: T, handle: (event: DolphinEventMap[T]) => void): () => void {
    const subscription = this.events.filter<DolphinEventMap[T]>((event) => event.type === eventType).subscribe(handle);
    return () => subscription.unsubscribe();
  }
}

export default function createMockDolphinClient(): DolphinService {
  return new MockDolphinClient();
}
