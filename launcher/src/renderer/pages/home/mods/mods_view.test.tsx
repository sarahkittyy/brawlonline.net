import type { ModBuild, ModRoomStatus, ModsState } from "@dolphin/mods/types";
import { cleanup, fireEvent, render } from "@testing-library/react";
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

const props = (state: ModsState | null, extra: Partial<ModsViewProps> = {}): ModsViewProps => ({
  state,
  publicRooms: { kind: "ready", rooms: [] },
  onLogIn: vi.fn(),
  hashProgress: {},
  startingBuildId: null,
  error: null,
  controller: "gc",
  onControllerChange: vi.fn(),
  onAddBuild: vi.fn(),
  onRemoveBuild: vi.fn(),
  onHost: vi.fn(),
  onJoin: vi.fn(),
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

  it("shows a build's short hash and hosts or joins with it", () => {
    const p = props({ builds: [minus], running: false, status: null });
    const { getByText, getByLabelText } = render(<ModsView {...p} />);
    expect(getByText("3f9a1c07")).toBeTruthy();
    fireEvent.click(getByText(Messages.host()));
    expect(p.onHost).toHaveBeenCalledWith(minus, true);
    fireEvent.change(getByLabelText(Messages.roomCode()), { target: { value: "kfqb" } });
    fireEvent.click(getByText(Messages.join()));
    expect(p.onJoin).toHaveBeenCalledWith(minus, "KFQB");
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
    const row = (name: string) => getByText(Messages.roomHost(name)).closest("div") as HTMLElement;
    const joinIn = (el: HTMLElement) =>
      Array.from(el.querySelectorAll("button")).find((b) => b.textContent === Messages.join()) as HTMLButtonElement;
    expect(joinIn(row("Dee")).disabled).toBe(true);
    fireEvent.click(joinIn(row("Keiko")));
    expect(p.onJoin).toHaveBeenCalledWith(minus, "PLNX");
    expect(getByText(Messages.roomPlayers(3, 4))).toBeTruthy();
  });

  it("asks to log in for the public rooms", () => {
    const p = props({ builds: [], running: false, status: null }, { publicRooms: { kind: "logged-out" } });
    const { getByText } = render(<ModsView {...p} />);
    fireEvent.click(getByText(Messages.logIn()));
    expect(p.onLogIn).toHaveBeenCalled();
  });
});
