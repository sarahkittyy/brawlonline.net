import { fireEvent, render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { PublicRoom, RoomRuleset } from "@/services/backend/types";

import { PublicRoomsView } from "./public_rooms_view";

const ruleset = (over: Partial<RoomRuleset>): RoomRuleset => ({
  code: null,
  name: "P+ default",
  version: 0,
  by: "",
  basedOn: null,
  bestOf: 0,
  stageChoice: "loser-picks",
  ...over,
});

const room = (code: string, r?: RoomRuleset): PublicRoom => ({
  code,
  host: code.toLowerCase(),
  players: 1,
  openSlots: 2,
  mode: "1v1",
  status: "waiting",
  names: [code.toLowerCase()],
  joinable: true,
  ruleset: r,
});

describe("the room list's ruleset column", () => {
  const rooms = [
    room("KFQB", ruleset({ code: "KFQBTR", name: "NorCal Weekly", version: 3, by: "sarah", bestOf: 3 })),
    room("PLNX", ruleset({ name: "NorCal Weekly", basedOn: "NorCal Weekly v3" })),
    room("HBPL", ruleset({})),
    room("MXWW"),
  ];

  it("names a published ruleset, says custom rules, and stays empty for P+ default", () => {
    const { container } = render(
      <PublicRoomsView
        state={{ kind: "ready", online: 4, rooms }}
        onJoin={() => undefined}
        onRetry={() => undefined}
        onLogIn={() => undefined}
        onShowRuleset={() => undefined}
      />,
    );
    const cell = (roomCode: string) =>
      container.querySelector(`[data-room="${roomCode}"] [class*="ruleset"]`)?.textContent ?? null;
    expect(cell("KFQB")).toBe("NorCal Weekly");
    expect(cell("PLNX")).toBe("Custom (based on NorCal Weekly v3)");
    expect(cell("HBPL")).toBe("");
    expect(cell("MXWW")).toBe("");
  });

  it("shows the ruleset on a click on its name, without joining the room", () => {
    const onJoin = vi.fn();
    const onShowRuleset = vi.fn();
    const { container } = render(
      <PublicRoomsView
        state={{ kind: "ready", online: 4, rooms }}
        onJoin={onJoin}
        onRetry={() => undefined}
        onLogIn={() => undefined}
        onShowRuleset={onShowRuleset}
      />,
    );
    fireEvent.click(container.querySelector('[data-ruleset-cell="KFQBTR"]')!);
    expect(onShowRuleset).toHaveBeenCalledWith(rooms[0]);
    expect(onJoin).not.toHaveBeenCalled();
    fireEvent.keyDown(container.querySelector('[data-ruleset-cell="KFQBTR"]')!, { key: "Enter" });
    expect(onShowRuleset).toHaveBeenCalledTimes(2);
    // Custom rules have no published ruleset to show; the row still joins.
    fireEvent.click(container.querySelector('[data-ruleset-cell="custom"]')!);
    expect(onShowRuleset).toHaveBeenCalledTimes(2);
    expect(onJoin).toHaveBeenCalledWith(rooms[1]);
  });
});
