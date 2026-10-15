import type {
  DolphinControllers,
  ModBuild,
  ModBuildInput,
  ModFolderParts,
  ModRoomRequest,
  ModRoomStatus,
  ModsController,
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
      id: "nsmbw",
      name: "New Super Mario Bros. Wii (USA, Rev 2)",
      discPath: "C:/Games/New Super Mario Bros. Wii.rvz",
      launcherPath: null,
      sdPath: null,
      gameId: "SMNE01",
      gameTitle: "New SUPER MARIO BROS. Wii",
      platform: "wii",
      hash: "8e21d0c3aa00000000000000000000000000000000000000000000000000a11a",
      hashKey: "1",
    },
    {
      id: "vanilla",
      name: "Super Smash Bros. Brawl",
      discPath: "C:/Games/Brawl.iso",
      launcherPath: null,
      sdPath: null,
      gameId: "RSBE01",
      gameTitle: "SUPER SMASH BROS. BRAWL",
      platform: "wii",
      hash: "3f9a1c07aa00000000000000000000000000000000000000000000000000beef",
      hashKey: "1",
    },
    {
      id: "minus",
      name: "Brawl Minus",
      discPath: "C:/Games/Brawl.iso",
      launcherPath: "C:/Games/Minus/Launcher/Minus Launcher.dol",
      sdPath: "C:/Games/Minus/User/Wii/sd.raw",
      gameId: "RSBE01",
      gameTitle: "SUPER SMASH BROS. BRAWL",
      platform: "wii",
      hash: null,
      hashKey: null,
    },
  ];
  private status: ModRoomStatus | null = null;
  private listeners = new Set<(event: ModsEvent) => void>();
  private hashing: Record<string, number | null> = {};
  private controller: ModsController = "gc";
  private readonly controllers: DolphinControllers = {
    gc: [{ index: 1, device: "GameCube adapter", label: "GameCube adapter" }],
    wiimote: [{ index: 1, device: "", label: "Real Wii Remote", real: true }],
  };

  private state(): ModsState {
    return {
      builds: this.builds,
      running: this.status !== null,
      status: this.status,
      lastError: null,
      controller: this.controller,
      controllers: this.controllers,
      hashing: { ...this.hashing },
    };
  }

  private emit() {
    const state = this.state();
    this.listeners.forEach((l) => l({ type: "state", state }));
  }

  async getState() {
    return this.state();
  }

  async addBuild(input: ModBuildInput) {
    const build: ModBuild = {
      ...input,
      name: input.name || input.discPath.split(/[\\/]/).pop() || "Game",
      id: String(Date.now()),
      gameId: "SMNE01",
      platform: "wii",
      hash: null,
      hashKey: null,
    };
    this.builds = [...this.builds, build];
    this.emit();
    return build;
  }

  async updateBuild(id: string, input: Partial<ModBuildInput>) {
    // An empty name goes back to the automatic one (here: the one it had).
    this.builds = this.builds.map((b) =>
      b.id === id ? { ...b, ...input, name: input.name || b.name, autoName: input.name === "" || b.autoName } : b,
    );
    this.emit();
    return this.builds.find((b) => b.id === id)!;
  }

  async inspectFolder(folder: string): Promise<ModFolderParts> {
    return {
      name: folder.split(/[\\/]/).pop() ?? "Mod",
      launcherPath: `${folder}/Launcher/Launcher.dol`,
      sdPath: `${folder}/User/Wii/sd.raw`,
    };
  }

  async removeBuild(id: string) {
    this.builds = this.builds.filter((b) => b.id !== id);
    this.emit();
  }

  async hashBuild(id: string) {
    this.hashing[id] = 0;
    this.emit();
    for (let step = 0; step <= 20; step++) {
      const fraction = step / 20;
      this.hashing[id] = fraction;
      this.listeners.forEach((l) => l({ type: "hashProgress", progress: { id, fraction } }));
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    delete this.hashing[id];
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
      input: { controller: this.controller, index: 1, device: "GameCube adapter", plugged: true, real: false },
      lastRequest: null,
      pid: 1,
      updatedAt: new Date().toISOString(),
    };
    this.emit();
  }

  async host(options: ModsHostOptions) {
    const build = await this.hashBuild(options.buildId);
    this.open(build, "KFQB", true, this.controller);
  }

  async join(options: ModsJoinOptions) {
    const build = await this.hashBuild(options.buildId);
    this.open(build, options.code.toUpperCase(), false, this.controller);
  }

  async joinByCode(options: { uid: string; code: string }) {
    const build = this.builds.find((b) => b.hash);
    if (!build || options.code.toUpperCase() === "NOPE") {
      return { outcome: "no-game" as const, name: "Some Mod", hash: "9".repeat(64) };
    }
    await this.join({ buildId: build.id, code: options.code });
    return { outcome: "joining" as const };
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

  async setController(controller: ModsController) {
    this.controller = controller;
    if (this.status) {
      const real = controller === "wiimote";
      this.status = {
        ...this.status,
        input: { controller, index: 1, device: real ? "" : "GameCube adapter", plugged: true, real },
      };
    }
    this.emit();
  }

  async refreshControllers() {
    // Nothing to read in dev mode.
  }

  async configureControllers() {
    // No Dolphin in dev mode.
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
