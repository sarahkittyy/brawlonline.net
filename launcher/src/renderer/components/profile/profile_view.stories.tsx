import Paper from "@mui/material/Paper";

import type { PlayerProfile } from "@/services/backend/types";

import { ProfileButtons } from "./profile_dialog";
import { ProfileView } from "./profile_view";

export default {
  title: "components/ProfileView",
  argTypes: {},
};

const gold: PlayerProfile = {
  uid: "them",
  displayName: "SonderMeaning",
  connectCode: "SOND#810",
  link: "https://brawlonline.net/profile/sond-810",
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
};

/** As the dialog shows it (the dialog's paper and padding). */
const Frame = ({ children }: { children: React.ReactNode }) => (
  <Paper sx={{ width: 444, padding: "24px 24px 20px" }}>{children}</Paper>
);

const Buttons = () => <ProfileButtons ignored={false} onMessage={() => undefined} onIgnore={() => undefined} />;

/** Someone else's profile: Message and Ignore user, and the web page. */
export const Placed = () => (
  <Frame>
    <ProfileView profile={gold} actions={<Buttons />} onCopyLink={() => undefined} />
  </Frame>
);

/** The top tier has no division. */
export const Legendary = () => (
  <Frame>
    <ProfileView
      profile={{
        ...gold,
        displayName: "Lucas",
        connectCode: "LUCA#5",
        link: "https://brawlonline.net/profile/luca-5",
        ranked: { ...gold.ranked, rank: { tier: "legendary", division: null }, rating: 2291.7, position: 1 },
      }}
      onCopyLink={() => undefined}
    />
  </Frame>
);

/** The player's own while placing: the rating is ????, no position, no buttons. */
export const Placing = () => (
  <Frame>
    <ProfileView
      profile={{
        ...gold,
        displayName: "newbie",
        connectCode: "NEWB#1",
        link: "https://brawlonline.net/profile/newb-1",
        ranked: {
          ...gold.ranked,
          rank: null,
          rating: null,
          setsPlayed: 2,
          setsToPlace: 3,
          wins: 1,
          losses: 1,
          position: null,
        },
      }}
      onCopyLink={() => undefined}
    />
  </Frame>
);

/** Never played Ranked. */
export const NotRanked = () => (
  <Frame>
    <ProfileView
      profile={{
        ...gold,
        displayName: "just joined",
        connectCode: "JUST#77",
        link: "https://brawlonline.net/profile/just-77",
        ranked: {
          ...gold.ranked,
          rank: null,
          rating: null,
          setsPlayed: 0,
          setsToPlace: 5,
          wins: 0,
          losses: 0,
          position: null,
        },
      }}
      actions={<Buttons />}
    />
  </Frame>
);

/** Mains (the primary first), a country and social media accounts. Stock icons need a disc: here name tags. */
export const Customized = () => (
  <Frame>
    <ProfileView
      profile={{
        ...gold,
        mains: ["fox", "falco", "random"],
        country: "se",
        socials: [
          { platform: "discord", handle: "sonder", display: "sonder", url: null },
          { platform: "twitch", handle: "sonder", display: "@sonder", url: "https://twitch.tv/sonder" },
        ],
      }}
      actions={<Buttons />}
      onCopyLink={() => undefined}
    />
  </Frame>
);

/** Edit mode on the player's own profile (nothing is saved here). */
export const Editing = () => (
  <Frame>
    <ProfileView
      profile={{ ...gold, mains: ["marth"], country: null, socials: [] }}
      editing={{ rename: async () => undefined, save: async () => undefined }}
      onCopyLink={() => undefined}
    />
  </Frame>
);
