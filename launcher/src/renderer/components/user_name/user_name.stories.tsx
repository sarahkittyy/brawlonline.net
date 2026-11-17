import { QueryClient, QueryClientProvider } from "@tanstack/react-query";

import { DEV_BADGE, STAR_BADGE, STORY_ICONS, TROPHY_BADGE } from "@/components/name_badges/story_badges";
import { createServiceProvider } from "@/services";
import type { BackendService, PlayerProfile } from "@/services/backend/types";
import type { Services } from "@/services/types";

import type { UserNameData } from "./user_name";
import { UserName } from "./user_name";

export default {
  title: "components/UserName",
  argTypes: {},
};

const players: UserNameData[] = [
  { uid: "a", displayName: "Sarah", connectCode: "SARA#123", rank: { tier: "gold", division: 2 } },
  { uid: "b", displayName: "Knuckles", connectCode: "KNUX#9", rank: { tier: "emerald", division: 3 } },
  { uid: "c", displayName: "Drsmsh", connectCode: "DRSM#41", rank: null },
  { uid: "d", displayName: "Lucas", connectCode: "LUCA#5", rank: { tier: "legendary", division: null } },
  { uid: "e", displayName: "Wario Fan", connectCode: "WARI#7", rank: { tier: "silver", division: 1 } },
];

/** As the match history shows names: the rank then, the code, the name opening the profile. */
export const WithBadges = () => (
  <div style={{ display: "flex", flexDirection: "column", gap: 8, padding: 16, fontSize: 14 }}>
    {players.map((p) => (
      <UserName key={p.uid} user={p} showBadge={true} profileLink={true} />
    ))}
  </div>
);

/** Name and code only (the online list, chat). */
export const Plain = () => (
  <div style={{ display: "flex", flexDirection: "column", gap: 8, padding: 16, fontSize: 13 }}>
    {players.map((p) => (
      <UserName key={p.uid} user={p} />
    ))}
  </div>
);

const badged: PlayerProfile = {
  uid: "a",
  displayName: "Sarah",
  connectCode: "SARA#123",
  link: null,
  ranked: {
    rank: { tier: "gold", division: 2 },
    rating: 1523.44,
    setsPlayed: 54,
    setsToPlace: 0,
    wins: 34,
    losses: 20,
    position: 12,
    rankedPlayers: 340,
  },
  mains: ["fox", "falco"],
  country: "se",
  badges: [DEV_BADGE, TROPHY_BADGE, STAR_BADGE],
  mainIcon: STORY_ICONS.heart,
};
const { ServiceProvider } = createServiceProvider({
  services: {
    backendService: { fetchProfile: async () => badged } as Partial<BackendService> as BackendService,
  } as Services,
});
const queryClient = new QueryClient();

/**
 * Badges an admin gave (docs/design/profiles.md §10) before the name, in a list (at most three) and cut short by a
 * narrow column: the name ends in "…", the badges stay. Hover a name for its card (badges, name, custom main icon).
 */
export const AdminBadges = () => (
  <QueryClientProvider client={queryClient}>
    <ServiceProvider>
      <div style={{ display: "flex", flexDirection: "column", gap: 8, padding: 16, fontSize: 14 }}>
        <UserName user={{ ...players[0], badges: badged.badges }} badgePlace="online" />
        <div style={{ width: 200, display: "flex" }}>
          <UserName
            user={{ uid: "a", displayName: "AVeryLongPlayerName", connectCode: "LONG#1", badges: badged.badges }}
            badgePlace="online"
          />
        </div>
      </div>
    </ServiceProvider>
  </QueryClientProvider>
);
