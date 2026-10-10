import { fireEvent, render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { PublicRoom } from "@/services/backend/types";

import { PublicRoomsMessages as Messages } from "./public_rooms.messages";
import type { PublicRoomsState } from "./public_rooms_view";
import { PublicRoomsView } from "./public_rooms_view";

const rooms: PublicRoom[] = [
  {
    code: "KFQB",
    host: "ann",
    players: 1,
    openSlots: 2,
    mode: "1v1",
    status: "waiting",
    names: ["ann"],
    joinable: true,
  },
  {
    code: "PLNX",
    host: "bob",
    players: 3,
    openSlots: 4,
    mode: "ffa",
    status: "in-game",
    names: ["bob", "cy", "dee"],
    joinable: true,
  },
  {
    code: "HBPL",
    host: "eve",
    players: 4,
    openSlots: 4,
    mode: "teams",
    status: "in-game",
    names: ["eve", "fay", "gus", "hal"],
    joinable: false,
  },
];

const view = (state: PublicRoomsState, onJoin = vi.fn(), joiningCode?: string) =>
  render(
    <PublicRoomsView state={state} joiningCode={joiningCode} onJoin={onJoin} onRetry={vi.fn()} onLogIn={vi.fn()} />,
  );

describe("PublicRoomsView", () => {
  it("lists every room with its host, players, mode, status and names", () => {
    const { container, getByTestId } = view({ kind: "ready", online: 42, rooms });
    expect(getByTestId("players-online").textContent).toBe("42 players online");
    const rows = container.querySelectorAll("button[data-room]");
    expect(rows.length).toBe(3);
    const text = (code: string) => container.querySelector(`[data-room="${code}"]`)!.textContent;
    expect(text("KFQB")).toBe("annann1v11/2WaitingJoin");
    expect(text("PLNX")).toContain("bob · cy · dee");
    expect(text("PLNX")).toContain("3/4");
    expect(text("PLNX")).toContain("In game");
    expect(text("HBPL")).toContain("Teams");
    expect(text("HBPL")).toContain("Full");
  });

  it("joins open rooms (also in game) but not full ones", () => {
    const onJoin = vi.fn();
    const { container } = view({ kind: "ready", online: 1, rooms }, onJoin);
    for (const code of ["KFQB", "PLNX", "HBPL"]) {
      fireEvent.click(container.querySelector(`[data-room="${code}"]`)!);
    }
    expect(onJoin.mock.calls.map((c) => c[0].code)).toEqual(["KFQB", "PLNX"]);
    expect(container.querySelector('[data-room="HBPL"]')!.getAttribute("aria-disabled")).toBe("true");
  });

  it("waits for one join at a time", () => {
    const onJoin = vi.fn();
    const { container } = view({ kind: "ready", online: 1, rooms }, onJoin, "KFQB");
    fireEvent.click(container.querySelector('[data-room="PLNX"]')!);
    expect(onJoin).not.toHaveBeenCalled();
  });

  it("shows the empty, logged-out and unreachable states", () => {
    expect(view({ kind: "ready", online: 1, rooms: [] }).getByText(Messages.noRooms())).toBeTruthy();
    expect(
      view({ kind: "ready", online: 1, rooms: [] }).getAllByText(Messages.createInGamePath()).length,
    ).toBeGreaterThan(0);
    expect(view({ kind: "ready", online: 1, rooms: [] }).getAllByText("1 player online").length).toBeGreaterThan(0);
    expect(view({ kind: "logged-out" }).getByText(Messages.logIn())).toBeTruthy();
    expect(view({ kind: "unreachable" }).getByText(Messages.retry())).toBeTruthy();
  });
});
