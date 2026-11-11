import type { SocialPlayer, SocialState } from "@social/types";
import { cleanup, render, screen, within } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";
import { useGameAssets } from "@/styles/game_theme";

import { OnlinePlayers } from "./online_players";

const players: SocialPlayer[] = [
  { uid: "a", displayName: "Lucas", connectCode: "LUCA#5", country: "se", main: "marth" },
  { uid: "b", displayName: "Ness", connectCode: "NESS#44" },
  { uid: "c", displayName: "Dice", connectCode: "DICE#1", main: "random" },
  // A character the disc's card has no stock icon for.
  { uid: "d", displayName: "Odd", connectCode: "ODD#2", main: "unknown_char" },
];

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

function renderList(disc: boolean) {
  useGameAssets
    .getState()
    .setState({ manifest: { stocks: disc ? { marth: { file: "theme/stocks/marth.png" } } : {} } } as any);
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
  const state: SocialState = {
    status: "online",
    online: players,
    room: null,
    ignored: [],
    invites: [],
    conversations: {},
  };
  useSocial.setState({ state, tabs: [] } as any);
  render(
    <ServiceProvider>
      <OnlinePlayers />
    </ServiceProvider>,
  );
}

const rowOf = (name: string) => screen.getByText(name).closest("[role=button]") as HTMLElement;

afterEach(() => {
  cleanup();
  useGameAssets.setState({ state: null });
});

describe("OnlinePlayers mains", () => {
  it("puts each main before the name, and a dot for players without one", () => {
    renderList(true);
    expect(within(rowOf("Lucas")).getByTestId("stock-marth")).toBeTruthy();
    expect(within(rowOf("Dice")).getByTestId("stock-random")).toBeTruthy();
    for (const name of ["Ness", "Odd"]) {
      const slot = within(rowOf(name)).getByTestId("online-main");
      expect(slot.querySelector("img, svg")).toBeNull();
      // Never the character's name tag (too wide for the slot).
      expect(slot.textContent).toBe("");
    }
    // The slot comes first, before the name.
    expect(rowOf("Lucas").firstElementChild?.getAttribute("data-testid")).toBe("online-main");
  });

  it("has no main column without a disc (no stock icons)", () => {
    renderList(false);
    expect(screen.queryAllByTestId("online-main")).toHaveLength(0);
    expect(rowOf("Lucas")).toBeTruthy();
  });
});
