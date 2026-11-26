import type { ChangelogMention } from "@accounts/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";

import { DEV_BADGE, STAR_BADGE, STORY_ICONS, TROPHY_BADGE } from "@/components/name_badges/story_badges";
import { createServiceProvider } from "@/services";
import type { BackendService, PlayerProfile } from "@/services/backend/types";
import type { Services } from "@/services/types";

import { ChangelogDialogView } from "./changelog_dialog_view";

export default {
  title: "lib/Changelog",
  argTypes: {},
};

const sarah: ChangelogMention = {
  uid: "24a184c8-608e-46b9-9e7b-d26d805c545c",
  displayName: "sarahkitty",
  connectCode: "SARA#694",
  country: "se",
  main: "random",
  badges: [DEV_BADGE],
};
const sosa: ChangelogMention = {
  uid: "6137184c-70b5-4759-8b7a-cf25dd5445ba",
  displayName: "sosa",
  connectCode: "SOSA#160",
  mainIcon: STORY_ICONS.heart,
  mainIconLabel: "Legendary",
  badges: [TROPHY_BADGE, STAR_BADGE],
};
const plain: ChangelogMention = {
  uid: "f6c5d568-2a91-47af-ac8a-c6afd8f7c328",
  displayName: "MidZ",
  connectCode: "MIDZ#71",
};

// As accounts renders the agent board's @ mentions (mentions.rs) and changelogHtml keeps them.
const html =
  "<h2>Launcher 0.2.11</h2>\n<ul>\n" +
  `<li>Thanks to <span data-mention="${sarah.uid}">@sarahkitty</span>, <span data-mention="${sosa.uid}">@sosa</span> ` +
  `and <span data-mention="${plain.uid}">@MidZ</span> for testing the new queue.</li>\n` +
  "<li>Fixed a freeze on the stage select screen (found by <span>@milla</span>, who is only on Discord).</li>\n</ul>\n" +
  `<p>A mention in a long paragraph wraps with the words around it, as <span data-mention="${sosa.uid}">@sosa</span> ` +
  "does here when the line runs out of room near the edge of the popup.</p>";

const profile: PlayerProfile = {
  uid: sarah.uid,
  displayName: "sarahkitty",
  connectCode: "SARA#694",
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
  mains: ["fox"],
  country: "se",
  badges: [DEV_BADGE],
};
const { ServiceProvider } = createServiceProvider({
  services: {
    backendService: { fetchProfile: async () => profile } as Partial<BackendService> as BackendService,
  } as Services,
});
const queryClient = new QueryClient();

/** Players the agent board's @ mentions: their main, flag, badges and name, hover for the card, click for the profile. */
export const Mentions = () => (
  <QueryClientProvider client={queryClient}>
    <ServiceProvider>
      <ChangelogDialogView
        open={true}
        entries={[{ id: 1, html, mentions: { [sarah.uid]: sarah, [sosa.uid]: sosa, [plain.uid]: plain } }]}
        hasMore={false}
        onLoadMore={() => undefined}
        onClose={() => undefined}
      />
    </ServiceProvider>
  </QueryClientProvider>
);
