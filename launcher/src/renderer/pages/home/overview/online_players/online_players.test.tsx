import type { SocialPlayer, SocialState } from "@social/types";
import { act, cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

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
  vi.restoreAllMocks();
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

describe("OnlinePlayers flags", () => {
  /**
   * jsdom has no layout: every row is `room` px wide, and while the list measures (rows at their natural width, flags
   * shown), Lucas's row (the one flagged player) is `lucas` px.
   */
  function fakeWidths(room: number, lucas: number) {
    vi.spyOn(HTMLElement.prototype, "getBoundingClientRect").mockImplementation(function (this: HTMLElement) {
      const measuring = this.closest("ul")?.className.includes("measure") ?? false;
      const width = measuring && this.textContent?.includes("Lucas") ? lucas : room;
      return { width, height: 30, top: 0, left: 0, right: width, bottom: 30, x: 0, y: 0 } as DOMRect;
    });
  }

  it("shows the flags while every flagged name fits beside its flag", () => {
    fakeWidths(220, 220);
    renderList(true);
    expect(screen.getByTestId("online-players").dataset.flags).toBe("shown");
    expect(rowOf("Lucas").querySelector("img[title], [class*=flag]")).toBeTruthy();
  });

  it("hides them all, keeping the invite, when a flagged name would not fit", () => {
    fakeWidths(220, 221);
    renderList(true);
    const list = screen.getByTestId("online-players");
    expect(list.dataset.flags).toBe("hidden");
    expect(list.className).toMatch(/hideFlags/);
    // The measuring class is gone again.
    expect(list.className).not.toMatch(/measure/);
    expect(within(rowOf("Lucas")).getByRole("button", { name: /invite/i })).toBeTruthy();
  });
});

describe("OnlinePlayers on an admin account", () => {
  const offline = [
    { uid: "a", displayName: "Lucas", connectCode: "LUCA#5", banned: false },
    { uid: "me", displayName: "Me", connectCode: "ME#1", banned: false },
    { uid: "x", displayName: "Sleepy", connectCode: "SLEP#9", country: "se", banned: false },
    { uid: "y", displayName: "Cheater", connectCode: "CHET#66", banned: true },
  ];

  function renderAdmin(admin: boolean) {
    const adminPlayers = vi.fn(async (q?: string) =>
      offline.filter((p) => !q || p.displayName.toLowerCase().includes(q) || p.connectCode.toLowerCase().includes(q)),
    );
    const { ServiceProvider: Provider } = createServiceProvider({
      services: { notificationService: toasts, backendService: { adminPlayers } } as unknown as Services,
    });
    useGameAssets.getState().setState({ manifest: { stocks: {} } } as any);
    useAccount.setState({ user: { uid: "me" }, userData: { playKey: {}, admin } } as any);
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
      <Provider>
        <OnlinePlayers />
      </Provider>,
    );
    return adminPlayers;
  }

  it("lists offline players after the online ones (not the admin, not anyone online), dimmed, without the envelope", async () => {
    renderAdmin(true);
    await screen.findByText("Sleepy");
    const rows = Array.from(screen.getByTestId("online-players").querySelectorAll("[role=button]"));
    const names = rows.map((r) => r.textContent ?? "");
    expect(names.map((n) => n.replace(/#.*/, "").replace(/[A-Z]+$/, ""))).toHaveLength(players.length + 2);
    expect(names.slice(0, players.length).every((n) => !n.includes("Sleepy"))).toBe(true);
    expect(names[players.length]).toContain("Sleepy");
    expect(names[players.length + 1]).toContain("Cheater");
    expect(screen.queryByText("Me")).toBeNull();
    expect(screen.getByTestId("online-offline-header").textContent).toBe("Offline");
    const sleepy = rowOf("Sleepy");
    expect(sleepy.className).toMatch(/offline/);
    expect(within(sleepy).queryByRole("button", { name: /invite/i })).toBeNull();
    expect(rowOf("Cheater").className).toMatch(/banned/);
  });

  it("searches offline players on the server too", async () => {
    const adminPlayers = renderAdmin(true);
    await screen.findByText("Sleepy");
    fireEvent.click(screen.getByTestId("online-search-toggle"));
    fireEvent.change(screen.getByTestId("online-search"), { target: { value: "chet" } });
    await waitFor(() => expect(adminPlayers).toHaveBeenLastCalledWith("chet"));
    await waitFor(() => expect(screen.queryByText("Sleepy")).toBeNull());
    expect(rowOf("Cheater")).toBeTruthy();
    expect(screen.queryByText("Lucas")).toBeNull();
  });

  it("shows only online players to anyone else, and never asks for the rest", async () => {
    const adminPlayers = renderAdmin(false);
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 20));
    });
    expect(adminPlayers).not.toHaveBeenCalled();
    expect(screen.queryByTestId("online-offline-header")).toBeNull();
  });
});
