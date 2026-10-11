import type { SocialPlayer, SocialState } from "@social/types";
import { useEffect } from "react";

import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { ChatDock } from "./chat_dock";

export default {
  title: "components/Social/ChatDock",
  argTypes: {},
};

const player = (uid: string, name: string, code: string): SocialPlayer => ({
  uid,
  displayName: name,
  connectCode: code,
});
const players = [player("bob", "Bob", "BOB#123"), player("kim", "Kim", "KIM#45"), player("lee", "Lee", "LEE#7")];

const state: SocialState = {
  status: "online",
  online: players,
  room: null,
  ignored: [],
  invites: [],
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
