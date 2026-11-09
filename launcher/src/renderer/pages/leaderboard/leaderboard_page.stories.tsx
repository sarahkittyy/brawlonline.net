import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import React from "react";

import { createServiceProvider } from "@/services";
import type { BackendService, LeaderboardEntry, RankTier } from "@/services/backend/types";
import type { Services } from "@/services/types";

import { LeaderboardPage } from "./leaderboard_page";

const tiers: RankTier[] = ["legendary", "master", "diamond", "emerald", "gold", "silver", "bronze"];
const players: LeaderboardEntry[] = Array.from({ length: 137 }, (_, i) => {
  const setsPlayed = 5 + ((i * 37) % 120);
  const wins = Math.round(setsPlayed * (0.75 - i / 400));
  return {
    position: i + 1,
    uid: `p${i + 1}`,
    displayName: `Player ${i + 1}`,
    connectCode: `PLYR#${100 + i}`,
    rating: 2300 - i * 10.5,
    setsPlayed,
    wins,
    losses: setsPlayed - wins,
    rank: {
      tier: tiers[Math.min(tiers.length - 1, Math.floor(i / 20))],
      division: i < 20 ? null : ((1 + (i % 3)) as 1 | 2 | 3),
    },
    setsToPlace: 0,
    positionChange: [0, 2, -1, 0, 5, -3, 1, 0, -1, 11, 1][i % 11],
  };
});

/** The server's search: name or code contains it, positions on the whole board, pages of 50. */
const backendService = {
  async fetchLeaderboard(after?: string, search?: string) {
    await new Promise((resolve) => setTimeout(resolve, 300));
    const q = search?.toLowerCase() ?? "";
    const rows = players.filter(
      (e) => e.displayName.toLowerCase().includes(q) || e.connectCode.toLowerCase().includes(q),
    );
    const start = after ? Number(after) : 0;
    return {
      entries: rows.slice(start, start + 50),
      next: start + 50 < rows.length ? String(start + 50) : null,
      total: players.length,
    };
  },
} as Partial<BackendService> as BackendService;
const services = { backendService } as Services;
const { ServiceProvider } = createServiceProvider({ services });

export default {
  title: "pages/Leaderboard",
  decorators: [
    (Story: React.FC) => (
      <QueryClientProvider client={new QueryClient()}>
        <ServiceProvider>
          <div style={{ display: "flex", height: 640, background: "var(--surface-0, #161616)" }}>
            <Story />
          </div>
        </ServiceProvider>
      </QueryClientProvider>
    ),
  ],
};

export const WholeBoard = () => <LeaderboardPage />;

/** Types `text` into the search box once the page is up, as a player would. */
const Searching = ({ text }: { text: string }) => {
  const ref = React.useRef<HTMLDivElement>(null);
  React.useEffect(() => {
    const input = ref.current?.querySelector<HTMLInputElement>("[data-testid=leaderboard-search]");
    if (input) {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set?.call(input, text);
      input.dispatchEvent(new Event("input", { bubbles: true }));
    }
  }, [text]);
  return (
    <div ref={ref} style={{ display: "flex", flex: 1, minWidth: 0 }}>
      <LeaderboardPage />
    </div>
  );
};

export const SearchByName = () => <Searching text="player 1" />;
export const SearchByCode = () => <Searching text="plyr#12" />;
export const NoMatch = () => <Searching text="zzz" />;
