import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { act, renderHook, waitFor } from "@testing-library/react";
import type { ReactNode } from "react";
import { afterEach, describe, expect, it, vi } from "vitest";

import type { PlayerProfile } from "@/services/backend/types";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

import { useMainPictures } from "./use_main_pictures";

const profile = (uid: string, mains?: string[]): PlayerProfile => ({
  uid,
  displayName: uid,
  connectCode: "",
  link: null,
  ranked: {
    rank: null,
    rating: null,
    setsPlayed: 0,
    setsToPlace: 5,
    wins: 0,
    losses: 0,
    position: null,
    rankedPlayers: 0,
  },
  mains,
});

const profiles: Record<string, PlayerProfile> = {
  marth: profile("marth", ["marth", "sheik"]),
  none: profile("none", []),
  old: profile("old"),
  random: profile("random", ["random", "fox"]),
};
const fetchProfile = vi.fn(async (uid: string) => profiles[uid]);
vi.mock("@/services", () => ({ useServices: () => ({ backendService: { fetchProfile } }) }));

const STOCKS = {
  marth: { file: "theme/stocks/marth.png" },
  sheik: { file: "theme/stocks/sheik.png" },
  fox: { file: "theme/stocks/fox.png" },
};

function withDisc(on: boolean) {
  useGameAssets.getState().setState({ manifest: { stocks: on ? STOCKS : {} } } as any);
}

function renderPictures(uids: string[], enabled = true) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const wrapper = ({ children }: { children: ReactNode }) => (
    <QueryClientProvider client={client}>{children}</QueryClientProvider>
  );
  return { client, ...renderHook(() => useMainPictures(uids, enabled), { wrapper }) };
}

afterEach(() => {
  fetchProfile.mockClear();
  useGameAssets.setState({ state: null });
});

describe("useMainPictures", () => {
  it("is the primary main's stock icon with a disc set up", async () => {
    withDisc(true);
    const { result } = renderPictures(["marth"]);
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/marth.png")));
  });

  it("keeps the default without a main, for Random, and without a disc", async () => {
    withDisc(true);
    const { result } = renderPictures(["none", "old", "random"]);
    await waitFor(() => expect(fetchProfile).toHaveBeenCalledTimes(3));
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(result.current).toEqual({});

    withDisc(false);
    const noDisc = renderPictures(["marth"]);
    await waitFor(() => expect(fetchProfile).toHaveBeenCalledWith("marth"));
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(noDisc.result.current).toEqual({});
  });

  it("follows a new primary saved in edit mode", async () => {
    withDisc(true);
    const { result, client } = renderPictures(["marth"]);
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/marth.png")));
    act(() => void client.setQueryData(["profile", "marth"], profile("marth", ["sheik", "marth"])));
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/sheik.png")));
  });

  it("fetches nothing while disabled", async () => {
    withDisc(true);
    const { result } = renderPictures(["marth"], false);
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(fetchProfile).not.toHaveBeenCalled();
    expect(result.current).toEqual({});
  });
});
