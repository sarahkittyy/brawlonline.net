import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import type { CSSProperties } from "react";
import { useEffect } from "react";

import { PlayersRail } from "@/app/shell/players_rail";
import { DEV_BADGE, STAR_BADGE, STORY_ICONS, TROPHY_BADGE } from "@/components/name_badges/story_badges";
import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";
import { useGameAssets } from "@/styles/game_theme";

import { OnlinePlayers } from "./online_players";

const queryClient = new QueryClient();

export default {
  title: "pages/Home/OnlinePlayers",
  argTypes: {},
  // The panel reads the rooms and live games (a friend's row says where they are).
  decorators: [
    (Story: React.ComponentType) => (
      <QueryClientProvider client={queryClient}>
        <Story />
      </QueryClientProvider>
    ),
  ],
};

const names = ["Bob", "Kim", "Lee", "Alexander", "Sora", "Bobby", "Marth Main", "kimchi", "Zelda", "Robin"];
// Some with a main shown, some without (a dot before the name), some with a flag.
const mains = ["fox", undefined, "sheik", "ike", undefined, "random", "marth", undefined, undefined, "wolf"];
const countries = ["us", "se", undefined, "jp", undefined, "us", "se", "br", "fr", undefined];
// Badges an admin gave (docs/design/profiles.md §10): Bob also has a custom main icon.
const badges = [
  [DEV_BADGE, TROPHY_BADGE],
  undefined,
  undefined,
  [STAR_BADGE],
  undefined,
  undefined,
  [TROPHY_BADGE, STAR_BADGE, DEV_BADGE],
];
const players: SocialPlayer[] = names.map((name, i) => ({
  uid: `p${i}`,
  displayName: name,
  connectCode: `${name.replace(/\W/g, "").slice(0, 4).toUpperCase()}#${i + 10}`,
  main: mains[i],
  country: countries[i],
  ...(badges[i] && { badges: badges[i] }),
  ...(i === 0 && { mainIcon: STORY_ICONS.heart }),
}));
const STOCKS = Object.fromEntries(
  ["fox", "sheik", "ike", "marth", "wolf"].map((c) => [c, { file: `stocks/${c}.png` }]),
);

function setUp() {
  useGameAssets.getState().setState({ manifest: { stocks: STOCKS } } as any);
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
  useSocial.setState({ state, tabs: [] });
}

const state: SocialState = {
  status: "online",
  online: players,
  room: null,
  ignored: [],
  invites: [],
  friends: { loaded: true, friends: [], incoming: [], outgoing: [] },
  requestPopups: [],
  conversations: {},
};

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

/** Ten players online: the magnifier next to the title opens the search box over the count. */
export const Default = () => {
  useEffect(setUp, []);
  return (
    <div style={{ width: 360, height: 420, display: "flex", flexDirection: "column", padding: 20 }}>
      <ServiceProvider>
        <OnlinePlayers />
      </ServiceProvider>
    </div>
  );
};

/**
 * The same list in the plain look and in P+'s colours (its menu gradient's palette, as
 * styles/game_theme.tsx makes it): the no-main dot has the accent's hue at a fixed lightness, so it is grey in one
 * and a muted green in the other.
 */
export const Themes = () => {
  useEffect(setUp, []);
  const pplus = {
    "--accent-primary": "#209a7a",
    "--accent-primary-bright": "#209a7a",
    "--accent-primary-dark": "#2773a1",
    "--surface-1": "#06211d",
    "--surface-2": "#092f2a",
    "--surface-3": "#2773a1",
    background: "#092f2a",
  } as CSSProperties;
  return (
    <ServiceProvider>
      <div style={{ display: "flex", gap: 20, padding: 20 }}>
        {[{}, pplus].map((vars, i) => (
          <div
            key={i}
            style={{ ...vars, width: 300, height: 460, display: "flex", flexDirection: "column", padding: 10 }}
          >
            <OnlinePlayers />
          </div>
        ))}
      </div>
    </ServiceProvider>
  );
};

// The names from the board's screenshot (bug: the list scrolled sideways in a narrow window).
const narrowPlayers: SocialPlayer[] = [
  ["E-C", "EC#319"],
  ["cartman", "KAY#709"],
  ["SanjiWatsuki", "SANJ#964"],
  ["TheCumGuzzla", "CUM#762"],
  ["Lhasheil [JPN]", "LSL#488", "jp", "random"],
  ["roy jones jr", "BOOM#216", "us", "marth"],
  ["Ozakai", "OZAK#924"],
  ["KoF_Rise", "HONE#802"],
].map(([displayName, connectCode, country, main], i) => ({
  uid: `n${i}`,
  displayName,
  connectCode,
  country,
  main,
  // Long names with badges: the name is cut short, the badges stay.
  ...(i === 2 && { badges: [DEV_BADGE, TROPHY_BADGE] }),
  ...(i === 4 && { badges: [STAR_BADGE] }),
}));

/**
 * The list at the overview column's width for a 1280, 1100 and 980 px window ((W - 72) / 4): flags show while every
 * flagged name fits beside its flag; narrower, they all hide and the envelopes stay at the right.
 */
export const Narrow = () => {
  useEffect(() => {
    setUp();
    useSocial.setState({ state: { ...state, online: narrowPlayers }, tabs: [] });
  }, []);
  return (
    <ServiceProvider>
      <div style={{ display: "flex", gap: 20, padding: 20, alignItems: "flex-start" }}>
        {[302, 257, 227].map((width) => (
          <div key={width} style={{ width, height: 380, display: "flex", flexDirection: "column" }}>
            <OnlinePlayers />
          </div>
        ))}
      </div>
    </ServiceProvider>
  );
};

/**
 * The players rail as every page has it (app/shell/players_rail.tsx, 272 px), with the redesign mockup's 185 online:
 * both tabs' counts beside either tab's icons.
 */
export const Rail = () => {
  useEffect(() => {
    setUp();
    useSocial.setState({
      state: {
        ...state,
        online: [
          ...players,
          ...Array.from({ length: 175 }, (_, i) => ({
            uid: `x${i}`,
            displayName: `Player ${i}`,
            connectCode: `PL#${i}`,
          })),
        ],
        friends: {
          loaded: true,
          friends: players.slice(0, 3).map((p) => ({ ...p, since: "2026-11-14T00:00:00Z" })),
          incoming: players.slice(3, 5).map((p) => ({ ...p, since: "2026-11-14T00:00:00Z" })),
          outgoing: [],
        },
      },
      tabs: [],
    });
  }, []);
  return (
    <ServiceProvider>
      <div style={{ display: "flex", height: 460 }}>
        <PlayersRail />
      </div>
    </ServiceProvider>
  );
};
