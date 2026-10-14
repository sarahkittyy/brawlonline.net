import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { MemoryRouter } from "react-router-dom";

import { LeaderboardRow } from "@/pages/leaderboard/leaderboard_page";
import { LeaderboardPageMessages } from "@/pages/leaderboard/leaderboard_page.messages";
import { createServiceProvider } from "@/services";
import type { LeaderboardEntry, Rank, RankedProfile, RankTier } from "@/services/backend/types";
import type { Services } from "@/services/types";

import { ContentBlock } from "../content_block/content_block";
import { RankedUserProfile } from "./ranked_user_profile";

const { ServiceProvider } = createServiceProvider({ services: {} as Services });
const queryClient = new QueryClient();

export default {
  title: "pages/Home/RankedBox",
  argTypes: {},
  decorators: [
    (Story: React.FC) => (
      <QueryClientProvider client={queryClient}>
        <ServiceProvider>
          <MemoryRouter>
            <div style={{ background: "var(--surface-0, #161616)", padding: 20 }}>
              <Story />
            </div>
          </MemoryRouter>
        </ServiceProvider>
      </QueryClientProvider>
    ),
  ],
};

const profile = (rating: number, extra: Partial<RankedProfile>): RankedProfile => ({
  rating,
  setsPlayed: 42,
  position: 23,
  rankedPlayers: 137,
  setsToPlace: 0,
  ...extra,
});

const BOXES: [string, RankedProfile][] = [
  ["Gold 2", profile(1351.2, { rank: { tier: "gold", division: 2 } })],
  ["Placing", profile(1463.8, { setsPlayed: 4, position: 61, rank: null, setsToPlace: 3 })],
  ["Placing, last set", profile(1512.0, { setsPlayed: 8, position: 40, rank: null, setsToPlace: 1 })],
  ["Bronze 3", profile(988.4, { setsPlayed: 17, position: 131, rank: { tier: "bronze", division: 3 } })],
  ["Diamond 1", profile(1903.5, { setsPlayed: 88, position: 9, rank: { tier: "diamond", division: 1 } })],
  ["Legendary", profile(2281.7, { setsPlayed: 136, position: 1, rank: { tier: "legendary", division: null } })],
];

/** The Home ranked box (MyRanking's ContentBlock) at a given size. */
const Box = ({ p, width, height }: { p: RankedProfile; width: number; height: number }) => (
  <div style={{ width }}>
    <ContentBlock
      overflowY="hidden"
      content={
        <div style={{ display: "flex", height, width: "100%" }}>
          <RankedUserProfile rankedProfile={p} onHide={() => undefined} />
        </div>
      }
    />
  </div>
);

const Boxes = ({ width, height }: { width: number; height: number }) => (
  <div style={{ display: "flex", flexWrap: "wrap", gap: 16 }}>
    {BOXES.map(([name, p]) => (
      <Box key={name} p={p} width={width} height={height} />
    ))}
  </div>
);

/** The sidebar at a 1280 px window (about 300 px wide), a 1920 px one (about 460 px), and a short window. */
export const Narrow = () => <Boxes width={300} height={200} />;
export const Wide = () => <Boxes width={460} height={200} />;
export const Short = () => <Boxes width={300} height={130} />;

const TIERS: RankTier[] = ["bronze", "silver", "gold", "emerald", "diamond", "master"];
const ALL: Rank[] = [
  { tier: "legendary", division: null },
  ...[...TIERS].reverse().flatMap((tier) => ([1, 2, 3] as const).map((division) => ({ tier, division }))),
];

// Each rank's lowest rating, Legendary first (common::ranked::RANK_FLOORS); Bronze 3's stands in at 1000.
const FLOORS = [
  1000, 1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1450, 1510, 1570, 1625, 1750, 1875, 2000, 2085, 2170, 2250,
];

/** Leaderboard rows with every rank, best first, and two players still placing. */
export const Leaderboard = () => {
  const rows: LeaderboardEntry[] = ALL.map((rank, i) => ({
    position: i + 1,
    uid: `p${i}`,
    displayName: ["Sarah", "Knuckles", "Drsmsh", "Wario Fan", "ONYX", "ANGE", "KARO", "DAD"][i % 8],
    connectCode: `PLYR#${100 + i}`,
    rating: FLOORS[FLOORS.length - 1 - i] + 20,
    setsPlayed: 30 + i,
    wins: 20,
    losses: 10 + i,
    rank,
    setsToPlace: 0,
  }));
  rows.splice(4, 0, { ...rows[4], uid: "placing-a", displayName: "NEWB", rank: null, setsToPlace: 3 });
  rows.splice(9, 0, { ...rows[9], uid: "placing-b", displayName: "Fresh", rank: null, setsToPlace: 1 });
  rows.forEach((r, i) => (r.position = i + 1));
  return (
    <div style={{ width: 900 }}>
      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell align="right" sx={{ width: 64 }}>
              {LeaderboardPageMessages.position()}
            </TableCell>
            <TableCell align="center" sx={{ width: 96 }}>
              {LeaderboardPageMessages.rank()}
            </TableCell>
            <TableCell>{LeaderboardPageMessages.player()}</TableCell>
            <TableCell>{LeaderboardPageMessages.connectCode()}</TableCell>
            <TableCell align="right">{LeaderboardPageMessages.rating()}</TableCell>
            <TableCell align="right">{LeaderboardPageMessages.record()}</TableCell>
          </TableRow>
        </TableHead>
        <TableBody>
          {rows.map((e) => (
            <LeaderboardRow key={e.uid} entry={e} isMe={e.uid === "p12"} />
          ))}
        </TableBody>
      </Table>
    </div>
  );
};
