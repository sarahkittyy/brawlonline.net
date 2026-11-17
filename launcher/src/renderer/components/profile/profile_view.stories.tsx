import Paper from "@mui/material/Paper";
import { useEffect } from "react";

import { DEV_BADGE, STAR_BADGE, STORY_ICONS, TROPHY_BADGE } from "@/components/name_badges/story_badges";
import type { PlayerProfile } from "@/services/backend/types";
import { useGameAssets } from "@/styles/game_theme";

import styles from "./profile.module.css";
import { EditPencil, OwnAccountButtons, ProfileButtons, ProfileCorner } from "./profile_dialog";
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

const STOCKS = Object.fromEntries(["fox", "falco"].map((c) => [c, { file: `stocks/${c}.png` }]));

/** As the dialog shows it (the dialog's paper, padding and X, with the player's own pencil beside it). */
const Frame = ({
  edit,
  stocks = false,
  children,
}: {
  edit?: React.ReactNode;
  /** Stock icons for the mains, as with a disc set up (the Storybook build has no extracted assets to show). */
  stocks?: boolean;
  children: React.ReactNode;
}) => {
  useEffect(() => {
    if (stocks) {
      useGameAssets.getState().setState({ manifest: { stocks: STOCKS } } as any);
    }
  }, [stocks]);
  return (
    <Paper sx={{ width: 444 }}>
      <div className={styles.dialog}>
        <ProfileCorner edit={edit} onClose={() => undefined} />
        {children}
      </div>
    </Paper>
  );
};

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

const placing: PlayerProfile = {
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
};

const Account = ({ activate = false, verify = false }: { activate?: boolean; verify?: boolean }) => (
  <OwnAccountButtons activate={activate} verify={verify} ruled={activate} onOpen={() => undefined} />
);

/**
 * The player's own while placing: the rating is ????, no position, no Message or Ignore; the pencil beside the X,
 * and Switch account and Log out under the web link (the header's name opens it).
 */
export const Placing = () => (
  <Frame edit={<EditPencil glow={false} onClick={() => undefined} />}>
    <ProfileView profile={placing} onCopyLink={() => undefined} />
    <Account />
  </Frame>
);

/** The player's own, before they have ever clicked the pencil on this client: it glows gold. */
export const OwnFirstTime = () => (
  <Frame edit={<EditPencil glow={true} onClick={() => undefined} />}>
    <ProfileView profile={placing} onCopyLink={() => undefined} />
    <Account />
  </Frame>
);

/** The player's own before online play is set up and the email verified: no code, no web page. */
export const OwnNotActivated = () => (
  <Frame edit={<EditPencil glow={false} onClick={() => undefined} />}>
    <ProfileView profile={{ ...placing, displayName: "brandnew", connectCode: "", link: null }} />
    <Account activate={true} verify={true} />
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

/**
 * Badges an admin gave, before the name (Dev on hover), and a custom main icon, the primary main: first of the mains,
 * the player's own after it (docs/design/profiles.md §10).
 */
export const Badges = () => (
  <Frame stocks={true}>
    <ProfileView
      profile={{
        ...gold,
        mains: ["fox", "falco"],
        country: "se",
        badges: [DEV_BADGE, TROPHY_BADGE, { ...STAR_BADGE, places: ["chat"] }],
        mainIcon: STORY_ICONS.heart,
        mainIconLabel: "Heart",
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

/** Edit mode with a custom main icon: it stays first; the picker edits only the player's own mains. */
export const EditingWithMainIcon = () => (
  <Frame stocks={true}>
    <ProfileView
      profile={{ ...gold, mains: ["fox", "falco"], country: "se", socials: [], mainIcon: STORY_ICONS.heart }}
      editing={{ rename: async () => undefined, save: async () => undefined }}
      onCopyLink={() => undefined}
    />
  </Frame>
);
