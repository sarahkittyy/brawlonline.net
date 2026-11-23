import { newRuleset } from "@common/rulesets";
import { fireEvent, render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { PublicRoom, RoomRuleset } from "@/services/backend/types";

import { PublicRoomsView } from "./public_rooms_view";
import { roomCustomRuleset } from "./room_ruleset";

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
    room("PLNX", ruleset({ name: "Custom", basedOn: "NorCal Weekly v3" })),
    room("CSTM", ruleset({ name: "Custom" })),
    room("HBPL", ruleset({})),
    room("MXWW"),
    // Custom rules with their rules (a server since the bug report): a click shows them.
    room(
      "RLZS",
      ruleset({
        name: "Custom",
        basedOn: "NorCal Weekly v3",
        bestOf: 3,
        stages: { starters: [1, 4, 9], counterpicks: [12] },
        strikes: [1, 1],
        bans: 2,
        dsr: true,
        rules: { ...newRuleset().rules, stocks: 3 },
      }),
    ),
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
    expect(cell("PLNX")).toBe("Custom"); // not what it was made from (the user's, 2026-10-12)
    expect(cell("CSTM")).toBe("Custom"); // P+ default edited
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
    // Custom rules from an older server came without their rules: nothing to show, the row still joins.
    fireEvent.click(container.querySelector('[data-room="PLNX"] [data-ruleset-cell="custom"]')!);
    expect(onShowRuleset).toHaveBeenCalledTimes(2);
    expect(onJoin).toHaveBeenCalledWith(rooms[1]);
    // With them, a click (or Enter) shows them, without joining.
    onJoin.mockClear();
    fireEvent.click(container.querySelector('[data-room="RLZS"] [data-ruleset-cell="custom"]')!);
    expect(onShowRuleset).toHaveBeenLastCalledWith(rooms[5]);
    fireEvent.keyDown(container.querySelector('[data-room="RLZS"] [data-ruleset-cell="custom"]')!, { key: "Enter" });
    expect(onShowRuleset).toHaveBeenCalledTimes(4);
    expect(onJoin).not.toHaveBeenCalled();
  });

  it("makes the ruleset a room's custom rules play", () => {
    expect(roomCustomRuleset(rooms[1])).toBeNull(); // an older server's: no rules
    expect(roomCustomRuleset(rooms[0])).toBeNull(); // published: looked up by its code
    const r = roomCustomRuleset(rooms[5])!;
    expect(r.rules.stocks).toBe(3);
    expect(r.stages).toEqual({ starters: [1, 4, 9], counterpicks: [12] });
    expect([r.bestOf, r.strikes, r.bans, r.dsr]).toEqual([3, [1, 1], 2, true]);
  });
});
