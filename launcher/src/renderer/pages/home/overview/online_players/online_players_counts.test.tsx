import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { OnlinePlayers, TAB_KEY } from "./online_players";

const players: SocialPlayer[] = [
  { uid: "a", displayName: "Lucas", connectCode: "LUCA#5" },
  { uid: "b", displayName: "Ness", connectCode: "NESS#44" },
];

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

function renderHeader(tab: "online" | "friends") {
  localStorage.setItem(TAB_KEY, JSON.stringify(tab));
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
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
  useSocial.setState({ state, tabs: [] } as any);
  render(
    <QueryClientProvider client={new QueryClient()}>
      <ServiceProvider>
        <OnlinePlayers />
      </ServiceProvider>
    </QueryClientProvider>,
  );
}

const countOf = (tab: "online" | "friends") =>
  screen.getByTestId(`online-tab-${tab}`).querySelector("[title]") as HTMLElement | null;

afterEach(() => {
  cleanup();
  localStorage.clear();
});

// Sarah's board: "the numbers should always show" (they hid on the tab not open when the rail was narrow).
describe("OnlinePlayers tab counts", () => {
  it.each(["online", "friends"] as const)("shows both tabs' counts with the %s tab open", (open) => {
    renderHeader(open);
    for (const [tab, count] of [
      ["online", "2"],
      ["friends", "0"],
    ] as const) {
      expect(countOf(tab)?.textContent).toBe(count);
      expect(countOf(tab)?.hasAttribute("data-hidden")).toBe(false);
    }
  });
});
