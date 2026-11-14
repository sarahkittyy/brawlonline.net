import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { PlayerMenu } from "./player_menu";

export default {
  title: "components/Social/PlayerMenu",
  argTypes: {},
};

const ness: SocialPlayer = { uid: "ness", displayName: "Ness", connectCode: "NESS#44" };

const state: SocialState = {
  status: "online",
  online: [ness],
  room: null,
  ignored: [],
  invites: [],
  friends: { loaded: true, friends: [], incoming: [], outgoing: [] },
  requestPopups: [],
  conversations: {},
};

const user = { uid: "ness", displayName: "Ness", connectCode: "NESS#44", muted: false, nameLocked: false };
const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: {
    notificationService: toasts,
    backendService: { adminUser: async () => ({ ...user, banned: false, admin: false }) },
  } as unknown as Services,
});
const queryClient = new QueryClient();

/** The menu on another player, as an admin sees it: every item with its icon. */
export const AsAdmin = () => {
  const [ready, setReady] = useState(false);
  useEffect(() => {
    useAccount.setState({ user: { uid: "me" }, userData: { playKey: {}, admin: true } } as any);
    useSocial.setState({ state, tabs: [] } as any);
    setReady(true);
  }, []);
  return (
    <QueryClientProvider client={queryClient}>
      <ServiceProvider>
        {ready && <PlayerMenu target={{ player: ness, x: 40, y: 40 }} onClose={() => {}} />}
      </ServiceProvider>
    </QueryClientProvider>
  );
};
