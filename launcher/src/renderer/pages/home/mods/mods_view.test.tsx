import type { ModBuild, ModRoomStatus, ModsState } from "@dolphin/mods/types";
import { cleanup, fireEvent, render, within } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { ModsMessages as Messages } from "./mods.messages";
import type { ModsViewProps } from "./mods_view";
import { ModsView } from "./mods_view";

const minus: ModBuild = {
  id: "minus",
  name: "Brawl Minus",
  discPath: "C:/Games/Brawl.iso",
  launcherPath: "C:/Games/Minus/minus.dol",
  sdPath: "C:/Games/Minus/sd.raw",
  hash: "3f9a1c07" + "0".repeat(56),
  hashKey: "k",
};

const room = (you: number): ModRoomStatus => ({
  version: 1,
  kind: you === 1 ? "host" : "join",
  state: "in",
  error: "",
  room: {
    code: "KFQB",
    public: false,
    build: { hash: minus.hash!, name: "Brawl Minus" },
    host: 1,
    you,
    slots: [
      { slot: 1, open: true, player: { displayName: "Ann", connectCode: "ANN#1", controller: "gc", connected: true } },
      {
        slot: 2,
        open: true,
        player: { displayName: "Bob", connectCode: "BOB#2", controller: "wiimote", connected: true },
      },
      { slot: 3, open: true },
      { slot: 4, open: false },
    ],
  },
  session: { phase: "running", players: 2, peers: [{ slot: 2, connected: true, rttMs: 41.6 }] },
  lastRequest: null,
  pid: 1,
  updatedAt: "2026-10-12T20:00:00Z",
});

const nsmbw: ModBuild = {
  id: "nsmbw",
  name: "New Super Mario Bros. Wii",
  discPath: "C:/Games/NSMBW.rvz",
  launcherPath: null,
  sdPath: null,
  gameId: "SMNE01",
  platform: "wii",
  hash: "bb372e26" + "0".repeat(56),
  hashKey: "k",
};

type ViewState = Omit<ModsState, "controller" | "controllers" | "lastError"> & Partial<ModsState>;

const props = (state: ViewState | null, extra: Partial<ModsViewProps> = {}): ModsViewProps => ({
  state: state && {
    controller: "gc",
    lastError: null,
    controllers: {
      gc: [{ index: 1, device: "XInput/0/Gamepad", label: "Xbox controller" }],
      wiimote: [{ index: 1, device: "", label: "Real Wii Remote", real: true }],
    },
    ...state,
  },
  publicRooms: { kind: "ready", rooms: [] },
  onLogIn: vi.fn(),
  hashProgress: {},
  startingBuildId: null,
  error: null,
  onControllerChange: vi.fn(),
  onConfigureControllers: vi.fn(),
  onRefreshControllers: vi.fn(),
  onAddBuild: vi.fn(),
  onAddFolder: vi.fn(),
  onRemoveBuild: vi.fn(),
  onHost: vi.fn(),
  onJoin: vi.fn(),
  onJoinCode: vi.fn(),
  onMissingGame: vi.fn(),
  onRequest: vi.fn(),
  onCopyCode: vi.fn(),
  ...extra,
});

describe("ModsView", () => {
  afterEach(cleanup);

  it("asks for a build when there is none", () => {
    const p = props({ builds: [], running: false, status: null });
    const { getByText } = render(<ModsView {...p} />);
    expect(getByText(Messages.noBuilds())).toBeTruthy();
    fireEvent.click(getByText(Messages.addBuild()));
    expect(p.onAddBuild).toHaveBeenCalled();
  });

  it("shows a build's short hash and hosts with it; one code box joins by code", () => {
    const p = props({ builds: [minus], running: false, status: null });
    const { getByText, getAllByLabelText } = render(<ModsView {...p} />);
    expect(getByText("3f9a1c07")).toBeTruthy();
    fireEvent.click(getByText(Messages.host()));
    expect(p.onHost).toHaveBeenCalledWith(minus, true);
    // One box, by the public rooms (not one per game).
    const boxes = getAllByLabelText(Messages.roomCode());
    expect(boxes).toHaveLength(1);
    fireEvent.change(boxes[0], { target: { value: "kfqb" } });
    fireEvent.keyDown(boxes[0], { key: "Enter" });
    expect(p.onJoinCode).toHaveBeenCalledWith("KFQB");
  });

  it("shows hashing progress and keeps the build's buttons off meanwhile", () => {
    const p = props(
      { builds: [{ ...minus, hash: null }], running: false, status: null },
      { hashProgress: { minus: 0.42 } },
    );
    const { getByText } = render(<ModsView {...p} />);
    expect(getByText(Messages.checking(42))).toBeTruthy();
    expect((getByText(Messages.host()).closest("button") as HTMLButtonElement).disabled).toBe(true);
  });

  it("shows the room to its host, with a kick for the other player and an open slot to close", () => {
    const p = props({ builds: [minus], running: true, status: room(1) });
    const { getByText, getAllByText } = render(<ModsView {...p} />);
    expect(getByText(Messages.inRoom("KFQB"))).toBeTruthy();
    expect(getByText("Bob")).toBeTruthy();
    expect(getByText(Messages.ping(41.6))).toBeTruthy();
    fireEvent.click(getByText(Messages.kick()));
    expect(p.onRequest).toHaveBeenCalledWith({ op: "kick", slot: 2 });
    fireEvent.click(getAllByText(Messages.closeSlot())[0]);
    expect(p.onRequest).toHaveBeenCalledWith({ op: "slot", slot: 3, open: false });
    fireEvent.click(getByText(Messages.leave()));
    expect(p.onRequest).toHaveBeenCalledWith({ op: "leave" });
    // Host and join are off while Dolphin runs a room.
    expect((getByText(Messages.host()).closest("button") as HTMLButtonElement).disabled).toBe(true);
  });

  it("gives a member no host controls", () => {
    const p = props({ builds: [minus], running: true, status: room(2) });
    const { queryByText } = render(<ModsView {...p} />);
    expect(queryByText(Messages.kick())).toBeNull();
    expect(queryByText(Messages.closeSlot())).toBeNull();
  });

  it("says why the session ended", () => {
    const status = room(2);
    status.session = { phase: "error", error: "Could not join: not admitted" };
    const p = props({ builds: [minus], running: true, status });
    const { getByText } = render(<ModsView {...p} />);
    expect(getByText(Messages.sessionError("Could not join: not admitted"))).toBeTruthy();
  });
  it("lists public rooms, with Join only for a build the player has", () => {
    const other = "9".repeat(64);
    const p = props(
      { builds: [minus], running: false, status: null },
      {
        publicRooms: {
          kind: "ready",
          rooms: [
            { code: "PLNX", build: { hash: minus.hash!, name: "Brawl Minus" }, hostName: "Keiko", players: 3, open: 4 },
            { code: "QRHQ", build: { hash: other, name: "Project M" }, hostName: "Dee", players: 1, open: 2 },
          ],
        },
      },
    );
    const { getByText } = render(<ModsView {...p} />);
    const row = (name: string) => getByText(Messages.roomHost(name)).closest("button") as HTMLButtonElement;
    // The whole row joins; a room whose game the player lacks says so, and a click explains.
    expect(row("Dee").textContent).toContain(Messages.noGame());
    fireEvent.click(row("Dee"));
    expect(p.onJoin).not.toHaveBeenCalled();
    expect(p.onMissingGame).toHaveBeenCalledWith("Project M", other);
    fireEvent.click(row("Keiko"));
    expect(p.onJoin).toHaveBeenCalledWith(minus, "PLNX");
    expect(getByText(Messages.roomPlayers(3, 4))).toBeTruthy();
  });

  it("switches the controller, folding what Dolphin has set up, and opens Configure", () => {
    const p = props({ builds: [minus], running: false, status: null });
    const { getByRole } = render(<ModsView {...p} />);
    fireEvent.mouseDown(getByRole("combobox", { name: Messages.controller() }));
    expect(p.onRefreshControllers).toHaveBeenCalled();
    const menu = within(getByRole("listbox"));
    expect(menu.getByText("Xbox controller")).toBeTruthy();
    expect(menu.getByText("Real Wii Remote")).toBeTruthy();
    fireEvent.click(menu.getByText(Messages.wiiRemote()));
    expect(p.onControllerChange).toHaveBeenCalledWith("wiimote");
    fireEvent.mouseDown(getByRole("combobox", { name: Messages.controller() }));
    fireEvent.click(within(getByRole("listbox")).getByText(Messages.configure()));
    expect(p.onConfigureControllers).toHaveBeenCalled();
    expect(p.onControllerChange).toHaveBeenCalledTimes(1);
    // Configure is not a controller: the menu still shows the one played.
    expect(getByRole("combobox", { name: Messages.controller() }).textContent).toBe(Messages.gameCube());
  });

  it("says what the player plays with in a room, and when it is missing", () => {
    const status = room(1);
    status.input = { controller: "wiimote", index: 1, device: "Bluetooth/0/Wii Remote", plugged: true, real: true };
    const p = props({ builds: [minus], running: true, status, controller: "wiimote" });
    const { getByText, rerender } = render(<ModsView {...p} />);
    expect(getByText(Messages.playingWith(Messages.realWiiRemote()))).toBeTruthy();
    status.input = { ...status.input, plugged: false };
    rerender(<ModsView {...props({ builds: [minus], running: true, status: { ...status }, controller: "wiimote" })} />);
    expect(getByText(Messages.notPluggedIn(Messages.wiiRemote()))).toBeTruthy();
  });

  it("warns that a Wii game may need a Wii Remote", () => {
    const status = room(1);
    status.room = { ...status.room!, build: { hash: nsmbw.hash!, name: nsmbw.name } };
    const p = props({ builds: [nsmbw], running: true, status });
    const { getByText } = render(<ModsView {...p} />);
    expect(getByText(Messages.gameNeedsWiiRemote())).toBeTruthy();
  });

  it("lists the player's own room among the public rooms, marked and not joinable", () => {
    const p = props(
      { builds: [minus], running: true, status: room(1) },
      {
        publicRooms: {
          kind: "ready",
          rooms: [
            { code: "KFQB", build: { hash: minus.hash!, name: "Brawl Minus" }, hostName: "Ann", players: 2, open: 3 },
          ],
        },
      },
    );
    const { getByText } = render(<ModsView {...p} />);
    const row = getByText(Messages.roomHost("Ann")).closest("button") as HTMLButtonElement;
    expect(row.textContent).toContain(Messages.yourRoomLabel());
    expect(row.getAttribute("aria-disabled")).toBe("true");
    fireEvent.click(row);
    expect(p.onJoin).not.toHaveBeenCalled();
  });

  it("keeps saying why a join failed after Dolphin closed", () => {
    const p = props({ builds: [minus], running: false, status: null, lastError: "Room not found." });
    const { getByText } = render(<ModsView {...p} />);
    expect(getByText("Room not found.")).toBeTruthy();
  });

  it("lets the host drag a player to another slot", () => {
    const p = props({ builds: [minus], running: true, status: room(1) });
    const { container } = render(<ModsView {...p} />);
    const row = (n: number) => container.querySelector(`[data-slot="${n}"]`) as HTMLElement;
    const data = { setData: vi.fn(), getData: () => "2", effectAllowed: "", dropEffect: "" };
    fireEvent.dragStart(row(2), { dataTransfer: data });
    fireEvent.dragOver(row(3), { dataTransfer: data });
    fireEvent.drop(row(3), { dataTransfer: data });
    expect(p.onRequest).toHaveBeenCalledWith({ op: "move", from: 2, to: 3 });
    // A closed, empty slot takes a player too, and a drop on its own slot moves nothing.
    fireEvent.dragStart(row(2), { dataTransfer: data });
    fireEvent.drop(row(4), { dataTransfer: data });
    expect(p.onRequest).toHaveBeenCalledWith({ op: "move", from: 2, to: 4 });
    fireEvent.dragStart(row(2), { dataTransfer: data });
    fireEvent.drop(row(2), { dataTransfer: data });
    expect(p.onRequest).toHaveBeenCalledTimes(2);
  });

  it("gives a member no dragging", () => {
    const p = props({ builds: [minus], running: true, status: room(2) });
    const { container } = render(<ModsView {...p} />);
    expect((container.querySelector('[data-slot="1"]') as HTMLElement).getAttribute("draggable")).toBe("false");
  });

  it("asks to log in for the public rooms", () => {
    const p = props({ builds: [], running: false, status: null }, { publicRooms: { kind: "logged-out" } });
    const { getByText } = render(<ModsView {...p} />);
    fireEvent.click(getByText(Messages.logIn()));
    expect(p.onLogIn).toHaveBeenCalled();
  });
});
