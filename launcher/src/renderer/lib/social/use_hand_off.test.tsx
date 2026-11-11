import type { RoomHandOffResult } from "@dolphin/room_handoff";
import { renderHook } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { useHandOff } from "./use_hand_off";

const joinRoomInGame = vi.fn<(code: string) => Promise<RoomHandOffResult>>();
const prepareRoomLaunch = vi.fn(async () => ({ id: "req" }));
const cancelRoomLaunch = vi.fn(async () => undefined);
vi.mock("@/services", () => ({
  useServices: () => ({ dolphinService: { joinRoomInGame, prepareRoomLaunch, cancelRoomLaunch } }),
}));

const showError = vi.fn();
const showInfo = vi.fn();
const showWarning = vi.fn();
vi.mock("@/lib/hooks/use_toasts", () => ({ useToasts: () => ({ showError, showInfo, showWarning }) }));

const play = vi.fn(async () => true);
vi.mock("@/lib/dolphin/use_play", () => ({ usePlay: () => play }));

beforeEach(() => {
  vi.clearAllMocks();
});

describe("useHandOff", () => {
  // Bug report #281: a room clicked while a Brawl Mods build (Cobalt, in Training) was open said
  // "going to the room" and nothing happened.
  it("tells the player to close a non-P+ game instead of saying it goes to the room", async () => {
    joinRoomInGame.mockResolvedValue({ outcome: "other-game", game: "Cobalt" });
    const { result } = renderHook(() => useHandOff());

    await expect(result.current("WXSC")).resolves.toBe(false);

    expect(showWarning).toHaveBeenCalledWith("Close Cobalt first, then try again.");
    expect(showInfo).not.toHaveBeenCalled();
    expect(play).not.toHaveBeenCalled();
    expect(prepareRoomLaunch).not.toHaveBeenCalled();
  });

  it("says it goes to the room when the game took it", async () => {
    joinRoomInGame.mockResolvedValue({ outcome: "accepted" });
    const { result } = renderHook(() => useHandOff());

    await expect(result.current("WXSC")).resolves.toBe(true);

    expect(showInfo).toHaveBeenCalledTimes(1);
    expect(showWarning).not.toHaveBeenCalled();
  });
});
