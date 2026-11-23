import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useEffect } from "react";

import { DEV_BADGE, STAR_BADGE, STORY_ICONS, TROPHY_BADGE } from "@/components/name_badges/story_badges";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { ChatDock } from "./chat_dock";

const queryClient = new QueryClient();

export default {
  title: "components/Social/ChatDock",
  argTypes: {},
  // The player menu (right-click a name) reads the account's friends.
  decorators: [
    (Story: React.ComponentType) => (
      <QueryClientProvider client={queryClient}>
        <Story />
      </QueryClientProvider>
    ),
  ],
};

const player = (uid: string, name: string, code: string): SocialPlayer => ({
  uid,
  displayName: name,
  connectCode: code,
});
const players = [
  // Badges and a custom main icon an admin gave (docs/design/profiles.md §10).
  { ...player("bob", "Bob", "BOB#123"), badges: [DEV_BADGE, TROPHY_BADGE], mainIcon: STORY_ICONS.heart },
  player("kim", "Kim", "KIM#45"),
  { ...player("lee", "Lee", "LEE#7"), badges: [STAR_BADGE] },
];

const state: SocialState = {
  status: "online",
  online: players,
  room: null,
  ignored: [],
  invites: [],
  friends: { loaded: true, friends: [], incoming: [], outgoing: [] },
  requestPopups: [],
  conversations: Object.fromEntries(
    players.map((p, i) => [
      p.uid,
      {
        player: p,
        unread: 0,
        online: true,
        lines: [
          { id: `${p.uid}-1`, own: false, text: `hi, it's ${p.displayName}`, at: i },
          { id: `${p.uid}-2`, own: true, text: "hey!", at: i + 1 },
        ],
      },
    ]),
  ),
};

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

/** Three open chats whose windows overlap: hover or click the tabs to bring each one up. */
export const OverlappingWindows = () => {
  useEffect(() => {
    (window as any).electron = {
      ...(window as any).electron,
      social: {
        markRead: async () => {},
        sendMessage: async () => ({ ok: true }),
        invite: async () => ({ ok: false }),
      },
    };
    useSocial.setState({ state, tabs: [] });
    for (const p of players) {
      useSocial.getState().openTab(p.uid, true);
    }
  }, []);
  return (
    <div style={{ height: "100vh", display: "flex", flexDirection: "column", justifyContent: "flex-end" }}>
      <ServiceProvider>
        <ChatDock />
      </ServiceProvider>
    </div>
  );
};
