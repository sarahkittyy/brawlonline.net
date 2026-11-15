import type { SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { PlayersRail } from "./players_rail";

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: {
    notificationService: toasts,
    backendService: { fetchRooms: async () => ({ online: 0, rooms: [], updatedAt: "" }) },
  } as unknown as Services,
});

function renderFolded(tab: "online" | "friends") {
  localStorage.setItem("PLAYERS_RAIL", JSON.stringify("folded"));
  localStorage.setItem("ONLINE_PANEL_TAB", JSON.stringify(tab));
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
  (window as any).electron = { ...(window as any).electron, spectate: { list: async () => ({ games: [] }) } };
  const state: SocialState = {
    status: "online",
    you: "me",
    online: [],
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
        <PlayersRail />
      </ServiceProvider>
    </QueryClientProvider>,
  );
}

const selected = (tab: string) => screen.getByTestId(`online-tab-${tab}`).getAttribute("aria-selected");

afterEach(() => {
  cleanup();
  localStorage.clear();
});

describe("PlayersRail folded", () => {
  it("opens on the Friends tab from the friends button", () => {
    renderFolded("online");
    fireEvent.click(screen.getByTestId("rail-open-friends"));
    expect(screen.getByTestId("players-rail")).toBeTruthy();
    expect(selected("friends")).toBe("true");
  });

  it("opens on the tab it had from the arrows", () => {
    renderFolded("online");
    fireEvent.click(screen.getByTestId("rail-open"));
    expect(selected("online")).toBe("true");
  });
});
