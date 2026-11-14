import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
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
      friends: { loaded: true, friends: [], incoming: [], outgoing: [] },
      requestPopups: [],
      conversations: {},
    };
    useSocial.setState({ state, tabs: [] } as any);
    render(
      <QueryClientProvider client={new QueryClient()}>
        <Provider>
          <OnlinePlayers />
        </Provider>
      </QueryClientProvider>,
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

describe("OnlinePlayers friends (docs/design/friends.md)", () => {
  const since = "2026-11-14T00:00:00Z";
  const friend = (p: SocialPlayer) => ({ ...p, since });
  const sendFriendRequest = vi.fn();

  function renderFriends(opts: { disc?: boolean; tab?: "online" | "friends" } = {}) {
    localStorage.setItem("ONLINE_PANEL_TAB", JSON.stringify(opts.tab ?? "online"));
    useGameAssets.getState().setState({
      manifest: { stocks: opts.disc === false ? {} : { marth: { file: "theme/stocks/marth.png" } } },
    } as any);
    useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
    (window as any).electron = {
      ...(window as any).electron,
      social: { sendFriendRequest, acceptFriend: vi.fn(), declineFriend: vi.fn(), cancelFriendRequest: vi.fn() },
      spectate: { list: async () => ({ games: [] }) },
    };
    const state: SocialState = {
      status: "online",
      you: "me",
      online: players,
      room: null,
      ignored: [],
      invites: [],
      friends: {
        loaded: true,
        // Lucas (a main) and Ness (none) online, Zed offline.
        friends: [
          friend(players[0]),
          friend(players[1]),
          { uid: "z", displayName: "Zed", connectCode: "ZED#9", since },
        ],
        incoming: [{ uid: "r", displayName: "Rae", connectCode: "RAE#3", since }],
        outgoing: [{ uid: "o", displayName: "Oli", connectCode: "OLI#4", since }],
      },
      requestPopups: [],
      conversations: {},
    };
    useSocial.setState({ state, tabs: [] } as any);
    const room = {
      code: "KFQB",
      host: "Lucas",
      players: 1,
      openSlots: 2,
      mode: "1v1",
      status: "waiting",
      names: ["Lucas"],
      uids: ["a"],
      joinable: true,
      name: "friday netplay",
      friends: true,
    };
    const { ServiceProvider: Provider } = createServiceProvider({
      services: {
        notificationService: toasts,
        backendService: { fetchRooms: async () => ({ online: 3, rooms: [room], updatedAt: "" }) },
      } as unknown as Services,
    });
    render(
      <QueryClientProvider client={new QueryClient()}>
        <Provider>
          <OnlinePlayers />
        </Provider>
      </QueryClientProvider>,
    );
  }

  it("puts friends first in Online, marked on their main or in the dot's place", () => {
    renderFriends();
    const rows = Array.from(screen.getByTestId("online-players").querySelectorAll("[role=button]"));
    const names = ["Lucas", "Ness", "Dice", "Odd"];
    expect(rows.map((r) => names.find((n) => r.textContent?.includes(n)))).toEqual(names);
    expect(within(rowOf("Lucas")).getByTestId("stock-marth")).toBeTruthy();
    expect(within(rowOf("Lucas")).getByTestId("online-friend")).toBeTruthy();
    expect(within(rowOf("Ness")).getByTestId("online-friend")).toBeTruthy();
    expect(within(rowOf("Dice")).queryByTestId("online-friend")).toBeNull();
    // The tabs: everyone online, friends online, and the requests waiting.
    expect(screen.getByTestId("online-tab-online").textContent).toBe("Online4");
    expect(screen.getByTestId("online-tab-friends").textContent).toBe("Friends21");
    expect(screen.getByTestId("friends-requests-badge").textContent).toBe("1");
  });

  it("marks a friend after the code without a disc", () => {
    renderFriends({ disc: false });
    expect(within(rowOf("Ness")).getByTestId("online-friend")).toBeTruthy();
    expect(screen.queryAllByTestId("online-main")).toHaveLength(0);
  });

  it("lists requests, friends online with what they do, offline friends and sent requests", async () => {
    renderFriends({ tab: "friends" });
    expect(within(screen.getByTestId("friend-incoming-r")).getByText("wants to be friends")).toBeTruthy();
    // Lucas is in a friends-only room the player can join.
    await screen.findByText("friday netplay · 1/2");
    expect(screen.getByTestId("friend-join-KFQB").textContent).toBe("JOIN");
    expect(within(screen.getByTestId("friend-online-b")).getByText("In the launcher")).toBeTruthy();
    expect(screen.getByTestId("friend-offline-z")).toBeTruthy();
    expect(within(screen.getByTestId("friend-outgoing-o")).getByText("Cancel")).toBeTruthy();
    // The offline ones fold.
    fireEvent.click(screen.getByTestId("friends-offline-toggle"));
    expect(screen.queryByTestId("friend-offline-z")).toBeNull();
  });

  it("sends a request from the code box, says what happened, and closes", async () => {
    sendFriendRequest.mockResolvedValue({
      ok: true,
      status: "sent",
      player: { uid: "q", displayName: "Quin", connectCode: "QUIN#1", since },
    });
    renderFriends({ tab: "friends" });
    fireEvent.click(screen.getByTestId("friends-add-toggle"));
    fireEvent.change(screen.getByTestId("friends-add-code"), { target: { value: "quin#1" } });
    fireEvent.click(screen.getByTestId("friends-add-send"));
    await waitFor(() =>
      expect(screen.getByTestId("friends-add-result").textContent).toBe("Request sent to Quin (QUIN#1)."),
    );
    expect(sendFriendRequest).toHaveBeenCalledWith("quin#1");
    fireEvent.click(screen.getByRole("button", { name: "Close" }));
    expect(screen.queryByTestId("friends-add-box")).toBeNull();
  });

  it("offers a request for a searched code that is no friend's", async () => {
    renderFriends({ tab: "friends" });
    fireEvent.click(screen.getByTestId("online-search-toggle"));
    fireEvent.change(screen.getByTestId("online-search"), { target: { value: "abc#12" } });
    expect((await screen.findByTestId("friends-search-request")).textContent).toBe("Send a friend request to ABC#12");
  });
});
