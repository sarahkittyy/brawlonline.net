import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

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

/**
 * jsdom has no layout: each tab is 40 px of name plus 36 px of count (its 6 px gap, dot and digits), 12 px apart, so
 * both counts need 164 px and the open tab's alone 128 px; `room` is what the header's icons leave the tabs.
 */
function renderHeader(room: number, tab: "online" | "friends" = "online") {
  localStorage.setItem(TAB_KEY, JSON.stringify(tab));
  vi.spyOn(HTMLElement.prototype, "getBoundingClientRect").mockImplementation(function (this: HTMLElement) {
    const width =
      this.getAttribute("role") === "tablist"
        ? room
        : this.getAttribute("role") === "tab"
        ? 76
        : this.parentElement?.getAttribute("role") === "tab" && this.hasAttribute("title")
        ? 30
        : 0;
    return { width, height: 20, top: 0, left: 0, right: width, bottom: 20, x: 0, y: 0, toJSON: () => ({}) };
  });
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

const countShown = (tab: "online" | "friends") => {
  const count = screen.getByTestId(`online-tab-${tab}`).querySelector("[title]") as HTMLElement;
  return !count.hasAttribute("data-hidden");
};

afterEach(() => {
  vi.restoreAllMocks();
  cleanup();
  localStorage.clear();
});

describe("OnlinePlayers tab counts", () => {
  it("shows both counts when they fit, exactly too", () => {
    renderHeader(164);
    expect(countShown("online")).toBe(true);
    expect(countShown("friends")).toBe(true);
  });

  it("drops the other tab's count first", () => {
    renderHeader(163);
    expect(countShown("online")).toBe(true);
    expect(countShown("friends")).toBe(false);
  });

  it("keeps the open tab's count, whichever it is", () => {
    renderHeader(128, "friends");
    expect(countShown("online")).toBe(false);
    expect(countShown("friends")).toBe(true);
  });

  it("drops both when even the open tab's doesn't fit", () => {
    renderHeader(127);
    expect(countShown("online")).toBe(false);
    expect(countShown("friends")).toBe(false);
  });
});
