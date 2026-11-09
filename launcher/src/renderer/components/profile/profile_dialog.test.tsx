import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, fireEvent, render, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useSocial } from "@/lib/social/use_social";
import type { PlayerProfile } from "@/services/backend/types";

import { ProfileDialog } from "./profile_dialog";

const gold: PlayerProfile = {
  uid: "them",
  displayName: "SonderMeaning",
  connectCode: "SOND#810",
  link: "https://brawlonline.net/profile/sond-810",
  ranked: {
    rank: { tier: "gold", division: 2 },
    rating: 1523.44,
    setsPlayed: 54,
    setsToPlace: 0,
    wins: 34,
    losses: 20,
    position: 12,
    rankedPlayers: 340,
  },
};

const placing: PlayerProfile = {
  ...gold,
  uid: "me",
  displayName: "newbie",
  connectCode: "NEWB#1",
  ranked: {
    ...gold.ranked,
    rank: null,
    rating: null,
    setsPlayed: 2,
    setsToPlace: 3,
    position: null,
    wins: 1,
    losses: 1,
  },
};

const profiles: Record<string, PlayerProfile> = { them: gold, me: placing };
const fetchProfile = vi.fn(async (id: string) => {
  const p = profiles[id];
  if (!p) {
    throw new Error("Not found");
  }
  return p;
});
vi.mock("@/services", () => ({ useServices: () => ({ backendService: { fetchProfile } }) }));

// Logged in as "me" with online play set up.
const account = { user: { uid: "me" }, userData: { playKey: { connectCode: "NEWB#1" } } };
vi.mock("@/lib/hooks/use_account", () => ({
  useAccount: (select: (s: typeof account) => unknown) => select(account),
}));

vi.mock("@/lib/hooks/use_toasts", () => ({
  useToasts: () => ({ showSuccess: () => undefined, showError: () => undefined, showInfo: () => undefined }),
}));

const setIgnored = vi.fn(async () => undefined);
Object.assign(window, { electron: { social: { setIgnored }, shell: { openExternal: vi.fn(async () => undefined) } } });

function open(uid: string) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const view = render(
    <QueryClientProvider client={client}>
      <ProfileDialog />
    </QueryClientProvider>,
  );
  useProfileView.getState().open({ uid });
  return view;
}

const stats = () =>
  Array.from(document.querySelectorAll("dl > div")).map(
    (d) => `${d.querySelector("dt")?.textContent}: ${d.querySelector("dd")?.textContent}`,
  );

describe("ProfileDialog", () => {
  afterEach(() => {
    cleanup();
    useProfileView.setState({ target: null });
    useSocial.setState({ tabs: [] });
  });

  it("shows someone's rank, Ranked numbers and web page, with Message and Ignore user", async () => {
    const { findByTestId, getByText, getByAltText } = open("them");
    expect((await findByTestId("profile-name")).textContent).toBe("SonderMeaning");
    expect(getByText("SOND#810")).toBeTruthy();
    expect(getByAltText("Gold 2")).toBeTruthy();
    expect(document.querySelector("[data-testid=profile-standing]")?.textContent).toBe("Gold 2");
    expect(stats()).toEqual(["Rating: 1523.4", "Leaderboard: #12 of 340", "Sets played: 54", "Record: 34W-20L"]);
    expect(getByText("brawlonline.net/profile/sond-810")).toBeTruthy();

    fireEvent.click(getByText("Ignore user"));
    expect(setIgnored).toHaveBeenCalledWith("them", true);

    // Message opens their chat window (they need not be online or have written before) and closes the profile.
    fireEvent.click(getByText("Message"));
    expect(useSocial.getState().tabs).toEqual([
      expect.objectContaining({
        uid: "them",
        open: true,
        player: { uid: "them", displayName: "SonderMeaning", connectCode: "SOND#810" },
      }),
    ]);
    await waitFor(() => expect(document.querySelector("[data-testid=profile-view]")).toBeNull());
  });

  it("shows the player's own profile while placing, without the buttons", async () => {
    const { findByTestId, queryByText } = open("me");
    await findByTestId("profile-view");
    expect(document.querySelector("[data-testid=profile-standing]")?.textContent).toBe("Placing: 3 more ranked sets");
    expect(stats()).toEqual(["Rating: ????", "Sets played: 2", "Record: 1W-1L"]);
    expect(queryByText("Message")).toBeNull();
    expect(queryByText("Ignore user")).toBeNull();
  });

  it("says so when the profile can't be loaded", async () => {
    const { findByText } = open("nobody");
    expect(await findByText("Couldn't load this profile. Try again later.")).toBeTruthy();
  });
});
