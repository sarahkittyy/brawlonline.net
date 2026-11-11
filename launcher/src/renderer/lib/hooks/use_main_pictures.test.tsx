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
const fetchProfile = vi.fn(async (uid: string): Promise<PlayerProfile> => {
  const p = profiles[uid];
  if (!p) {
    throw new Error("Not found");
  }
  return p;
});
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

  it("is none without a main, for Random, without a disc, and when the fetch fails", async () => {
    withDisc(true);
    const { result } = renderPictures(["none", "old", "random", "missing"]);
    await waitFor(() => expect(result.current).toEqual({ none: null, old: null, random: null, missing: null }));

    withDisc(false);
    const noDisc = renderPictures(["marth"]);
    await waitFor(() => expect(noDisc.result.current).toEqual({ marth: null }));
  });

  it("is unknown while the profile or the game assets load", async () => {
    let answer: (p: PlayerProfile) => void = () => undefined;
    fetchProfile.mockImplementationOnce(() => new Promise((resolve) => (answer = resolve)));
    const { result } = renderPictures(["marth"]);
    await waitFor(() => expect(fetchProfile).toHaveBeenCalled());
    expect(result.current).toEqual({});
    act(() => answer(profiles.marth));
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(result.current).toEqual({});
    act(() => withDisc(true));
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/marth.png")));
  });

  it("follows a new primary saved in edit mode", async () => {
    withDisc(true);
    const { result, client } = renderPictures(["marth"]);
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/marth.png")));
    act(() => void client.setQueryData(["profile", "marth"], profile("marth", ["sheik", "marth"])));
    await waitFor(() => expect(result.current.marth).toBe(assetUrl("theme/stocks/sheik.png")));
  });

  it("fetches nothing while disabled (offline): none", async () => {
    withDisc(true);
    const { result } = renderPictures(["marth"], false);
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(fetchProfile).not.toHaveBeenCalled();
    expect(result.current).toEqual({ marth: null });
  });
});
