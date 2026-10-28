import { act, cleanup, fireEvent, render } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

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

afterEach(cleanup);

describe("PublicRoomsView", () => {
  it("lists every room with its host, players, mode, status and names", () => {
    const { container, getByTestId } = view({ kind: "ready", online: 42, rooms });
    expect(getByTestId("players-online").textContent).toBe("42 players in-game");
    const rows = container.querySelectorAll("button[data-room]");
    expect(rows.length).toBe(3);
    const text = (code: string) => container.querySelector(`[data-room="${code}"]`)!.textContent;
    expect(text("KFQB")).toBe("ann's room♛ ann1v11/2WaitingJoin");
    expect(text("PLNX")).toContain("bob · cy · dee");
    expect(text("PLNX")).toContain("3/4");
    expect(text("PLNX")).toContain("In game");
    expect(text("HBPL")).toContain("Teams");
    expect(text("HBPL")).toContain("Full");
  });

  it("shows a room's name, or its host's room in grey, and crowns the host", () => {
    const named = [{ ...rooms[1], name: "chill ffa", names: ["cy", "bob", "dee"] }, rooms[0]];
    const { container } = view({ kind: "ready", online: 1, rooms: named });
    const title = (code: string) => container.querySelector(`[data-room="${code}"] > div > div`)!;
    expect(title("PLNX").textContent).toBe("chill ffa");
    expect(title("KFQB").textContent).toBe("ann's room");
    expect(title("KFQB").className).toMatch(/unnamed/);
    expect(title("PLNX").className).not.toMatch(/unnamed/);
    expect(container.querySelector('[data-room="PLNX"]')!.textContent).toContain("cy · ♛ bob · dee");
  });

  it("pins the player's room first, which its host names in place", async () => {
    const onRename = vi.fn(() => Promise.resolve());
    const onJoin = vi.fn();
    const own = (host: boolean) => (
      <PublicRoomsView
        state={{ kind: "ready", online: 1, rooms }}
        onJoin={onJoin}
        onRetry={vi.fn()}
        onLogIn={vi.fn()}
        yourRoom={{ kind: "pplus", code: "PLNX", host, name: null }}
        onRenameRoom={onRename}
      />
    );
    const { container, getByTestId, queryByTestId, rerender } = render(own(true));
    const first = container.querySelector("[data-room]")!;
    expect(first.getAttribute("data-room")).toBe("PLNX");
    expect(first.tagName).toBe("DIV");
    expect(first.textContent).toContain("Yours");
    fireEvent.click(first);
    expect(onJoin).not.toHaveBeenCalled();

    // Esc goes back without saving.
    fireEvent.click(getByTestId("room-name-pencil"));
    fireEvent.change(getByTestId("room-name-input"), { target: { value: "nope" } });
    fireEvent.keyDown(getByTestId("room-name-input"), { key: "Escape" });
    expect(queryByTestId("room-name-input")).toBeNull();
    expect(onRename).not.toHaveBeenCalled();

    // A click on the name opens it too, the player names stay under it; Enter saves.
    fireEvent.click(getByTestId("room-name"));
    expect(getByTestId("room-name-input")).toBeTruthy();
    expect(first.textContent).toContain("♛ bob");
    expect(first.textContent).not.toContain("Enter saves");
    fireEvent.change(getByTestId("room-name-input"), { target: { value: "  friday   doubles " } });
    await act(async () => {
      fireEvent.keyDown(getByTestId("room-name-input"), { key: "Enter" });
    });
    expect(onRename).toHaveBeenCalledWith("PLNX", "friday doubles");
    expect(queryByTestId("room-name-input")).toBeNull();

    // A member who doesn't host it has no pencil.
    rerender(own(false));
    expect(queryByTestId("room-name-pencil")).toBeNull();
    fireEvent.click(getByTestId("room-name"));
    expect(queryByTestId("room-name-input")).toBeNull();
  });

  it("keeps the box open with the text when the server refuses the name", async () => {
    const onRename = vi.fn(() => Promise.reject(new Error("Room names are at most 32 characters.")));
    const { getByTestId } = render(
      <PublicRoomsView
        state={{ kind: "ready", online: 1, rooms }}
        onJoin={vi.fn()}
        onRetry={vi.fn()}
        onLogIn={vi.fn()}
        yourRoom={{ kind: "pplus", code: "KFQB", host: true, name: null }}
        onRenameRoom={onRename}
      />,
    );
    fireEvent.click(getByTestId("room-name-pencil"));
    fireEvent.change(getByTestId("room-name-input"), { target: { value: "too long" } });
    await act(async () => {
      fireEvent.keyDown(getByTestId("room-name-input"), { key: "Enter" });
    });
    expect(onRename).toHaveBeenCalledTimes(1);
    expect((getByTestId("room-name-input") as HTMLInputElement).value).toBe("too long");
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
    expect(
      render(
        <PublicRoomsView
          state={{ kind: "ready", online: 1, rooms: [] }}
          onJoin={vi.fn()}
          onRetry={vi.fn()}
          onLogIn={vi.fn()}
          onCreate={vi.fn()}
        />,
      ).getByText(Messages.createAbove()),
    ).toBeTruthy();
    expect(view({ kind: "ready", online: 1, rooms: [] }).getAllByText("1 player in-game").length).toBeGreaterThan(0);
    expect(view({ kind: "logged-out" }).getByText(Messages.logIn())).toBeTruthy();
    expect(view({ kind: "unreachable" }).getByText(Messages.retry())).toBeTruthy();
  });

  const codeView = (onJoinCode: (code: string) => Promise<boolean> | void, state?: PublicRoomsState) =>
    render(
      <PublicRoomsView
        state={state ?? { kind: "ready", online: 1, rooms }}
        onJoin={vi.fn()}
        onJoinCode={onJoinCode}
        onRetry={vi.fn()}
        onLogIn={vi.fn()}
        onCreate={vi.fn()}
      />,
    );

  it("joins a typed room code, uppercased, with Join or Enter, once it is a room code", async () => {
    const onJoinCode = vi.fn().mockResolvedValue(false);
    const { getByTestId } = codeView(onJoinCode);
    const box = getByTestId("room-code") as HTMLInputElement;
    const button = getByTestId("join-code") as HTMLButtonElement;
    expect(button.disabled).toBe(true);

    fireEvent.change(box, { target: { value: "k f-q" } });
    expect(box.value).toBe("KFQ");
    expect(button.disabled).toBe(true);
    fireEvent.keyDown(box, { key: "Enter" });
    expect(onJoinCode).not.toHaveBeenCalled();

    // Four letters with a vowel (or a Y) are no room's code: red, no join.
    fireEvent.change(box, { target: { value: "KFQA" } });
    expect(box.getAttribute("aria-invalid")).toBe("true");
    expect(button.disabled).toBe(true);
    fireEvent.keyDown(box, { key: "Enter" });
    expect(onJoinCode).not.toHaveBeenCalled();

    fireEvent.change(box, { target: { value: "kfqb" } });
    expect(box.getAttribute("aria-invalid")).toBe("false");
    await act(async () => {
      fireEvent.keyDown(box, { key: "Enter" });
    });
    await act(async () => {
      fireEvent.click(button);
    });
    expect(onJoinCode.mock.calls).toEqual([["KFQB"], ["KFQB"]]);
    // Not taken by the game: the code stays to try again.
    expect(box.value).toBe("KFQB");
  });

  it("clears the code once the game takes it", async () => {
    const { getByTestId } = codeView(vi.fn().mockResolvedValue(true));
    const box = getByTestId("room-code") as HTMLInputElement;
    fireEvent.change(box, { target: { value: "MXWW" } });
    await act(async () => {
      fireEvent.click(getByTestId("join-code"));
    });
    expect(box.value).toBe("");
  });

  it("offers the code box with the list unreachable, not logged out", () => {
    expect(codeView(vi.fn(), { kind: "unreachable" }).queryByTestId("room-code")).toBeTruthy();
    cleanup();
    expect(codeView(vi.fn(), { kind: "logged-out" }).queryByTestId("room-code")).toBeNull();
  });
});
