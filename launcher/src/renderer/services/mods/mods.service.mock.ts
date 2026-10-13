import type {
  ModBuild,
  ModBuildInput,
  ModRoomRequest,
  ModRoomStatus,
  ModsEvent,
  ModsHostOptions,
  ModsJoinOptions,
  ModsService,
  ModsState,
} from "@dolphin/mods/types";

/** Dev mode (`npm run dev -- --env mock=mods`): a fake room that a click opens, with two players. */
class MockModsClient implements ModsService {
  private builds: ModBuild[] = [
    {
      id: "vanilla",
      name: "Brawl (vanilla)",
      discPath: "C:/Games/Brawl.iso",
      launcherPath: null,
      sdPath: null,
      hash: "3f9a1c07aa00000000000000000000000000000000000000000000000000beef",
      hashKey: "1",
    },
    {
      id: "minus",
      name: "Brawl Minus",
      discPath: "C:/Games/Brawl.iso",
      launcherPath: "C:/Games/Minus/MinusLauncher/minus.dol",
      sdPath: "C:/Games/Minus/sd.raw",
      hash: null,
      hashKey: null,
    },
  ];
  private status: ModRoomStatus | null = null;
  private listeners = new Set<(event: ModsEvent) => void>();

  private state(): ModsState {
    return { builds: this.builds, running: this.status !== null, status: this.status };
  }

  private emit() {
    const state = this.state();
    this.listeners.forEach((l) => l({ type: "state", state }));
  }

  async getState() {
    return this.state();
  }

  async addBuild(input: ModBuildInput) {
    const build: ModBuild = { ...input, id: String(Date.now()), hash: null, hashKey: null };
    this.builds = [...this.builds, build];
    this.emit();
    return build;
  }

  async updateBuild(id: string, input: Partial<ModBuildInput>) {
    this.builds = this.builds.map((b) => (b.id === id ? { ...b, ...input } : b));
    this.emit();
    return this.builds.find((b) => b.id === id)!;
  }

  async removeBuild(id: string) {
    this.builds = this.builds.filter((b) => b.id !== id);
    this.emit();
  }

  async hashBuild(id: string) {
    const total = 8_500_000_000;
    for (let done = 0; done <= total; done += total / 20) {
      this.listeners.forEach((l) => l({ type: "hashProgress", progress: { id, done, total } }));
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    this.builds = this.builds.map((b) => (b.id === id ? { ...b, hash: "a".repeat(64), hashKey: "1" } : b));
    this.emit();
    return this.builds.find((b) => b.id === id)!;
  }

  private open(build: ModBuild, code: string, isHost: boolean, controller: string) {
    this.status = {
      version: 1,
      kind: isHost ? "host" : "join",
      state: "in",
      error: "",
      room: {
        code,
        public: true,
        build: { hash: build.hash ?? "", name: build.name },
        host: 1,
        you: isHost ? 1 : 2,
        slots: [
          {
            slot: 1,
            open: true,
            player: { displayName: "Sarah", connectCode: "SARA#123", controller: "gc", connected: true },
          },
          {
            slot: 2,
            open: true,
            player: isHost ? undefined : { displayName: "You", connectCode: "YOU#1", controller, connected: true },
          },
          { slot: 3, open: true },
          { slot: 4, open: false },
        ],
      },
      session: {
        phase: "running",
        players: isHost ? 1 : 2,
        epoch: isHost ? 0 : 1,
        frame: 4000,
        slot: isHost ? 0 : 1,
        peers: [],
      },
      lastRequest: null,
      pid: 1,
      updatedAt: new Date().toISOString(),
    };
    this.emit();
  }

  async host(options: ModsHostOptions) {
    const build = await this.hashBuild(options.buildId);
    this.open(build, "KFQB", true, options.controller);
  }

  async join(options: ModsJoinOptions) {
    const build = await this.hashBuild(options.buildId);
    this.open(build, options.code.toUpperCase(), false, options.controller);
  }

  async request(request: ModRoomRequest) {
    if (!this.status?.room) {
      return;
    }
    if (request.op === "leave") {
      this.status = null;
    } else if (request.op === "public") {
      this.status = { ...this.status, room: { ...this.status.room, public: request.public } };
    } else if (request.op === "slot" || request.op === "kick") {
      const open = request.op === "slot" ? request.open : true;
      this.status = {
        ...this.status,
        room: {
          ...this.status.room,
          slots: this.status.room.slots.map((s) =>
            s.slot === request.slot ? { slot: s.slot, open, player: request.op === "kick" ? undefined : s.player } : s,
          ),
        },
      };
    }
    this.emit();
  }

  onEvent(handle: (event: ModsEvent) => void) {
    this.listeners.add(handle);
    return () => {
      this.listeners.delete(handle);
    };
  }
}

export default function createMockModsClient(): ModsService {
  return new MockModsClient();
}
