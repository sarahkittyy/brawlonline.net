import type { SocialPlayer, SocialState } from "@social/types";
import { useEffect } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { OnlinePlayers } from "./online_players";

export default {
  title: "pages/Home/OnlinePlayers",
  argTypes: {},
};

const names = ["Bob", "Kim", "Lee", "Alexander", "Sora", "Bobby", "Marth Main", "kimchi", "Zelda", "Robin"];
const players: SocialPlayer[] = names.map((name, i) => ({
  uid: `p${i}`,
  displayName: name,
  connectCode: `${name.replace(/\W/g, "").slice(0, 4).toUpperCase()}#${i + 10}`,
}));

const state: SocialState = {
  status: "online",
  online: players,
  room: null,
  ignored: [],
  invites: [],
  conversations: {},
};

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

/** Ten players online: the magnifier next to the title opens the search box over the count. */
export const Default = () => {
  useEffect(() => {
    useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
    useSocial.setState({ state, tabs: [] });
  }, []);
  return (
    <div style={{ width: 360, height: 420, display: "flex", flexDirection: "column", padding: 20 }}>
      <ServiceProvider>
        <OnlinePlayers />
      </ServiceProvider>
    </div>
  );
};
