import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { act, fireEvent, render, waitFor } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { LeaderboardEntry, LeaderboardPage as Page } from "@/services/backend/types";

import { LeaderboardPage } from "./leaderboard_page";

const entry = (position: number, displayName: string, connectCode: string): LeaderboardEntry => ({
  position,
  uid: `u${position}`,
  displayName,
  connectCode,
  rating: 2000 - position,
  setsPlayed: 10,
  wins: 5,
  losses: 5,
  setsToPlace: 0,
});
const everyone = [entry(1, "ann", "ANN#1"), entry(2, "bob", "BOB#2"), entry(3, "annie", "ANNI#3")];

const calls: { after?: string; search?: string }[] = [];
const fetchLeaderboard = vi.fn(async (after?: string, search?: string): Promise<Page> => {
  calls.push({ after, search });
  const q = search?.toLowerCase() ?? "";
  const entries = everyone.filter((e) => e.displayName.includes(q) || e.connectCode.toLowerCase().includes(q));
  return { entries, next: null, total: everyone.length };
});

vi.mock("@/services", () => ({
  useServices: () => ({ backendService: { fetchLeaderboard, fetchFriendsLeaderboard: vi.fn() } }),
}));
// Logged out unless a test logs in as one of `everyone`.
let account: { user: { uid: string } | null; userData?: { playKey: { connectCode: string } } } = { user: null };
vi.mock("@/lib/hooks/use_account", () => ({
  useAccount: (select: (s: typeof account) => unknown) => select(account),
}));

function renderPage() {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, refetchOnWindowFocus: false } } });
  return render(
    <QueryClientProvider client={client}>
      <LeaderboardPage />
    </QueryClientProvider>,
  );
}

const names = (container: HTMLElement) =>
  // The player cell holds the flag, the name and the code (the name is the code's sibling before it).
  Array.from(container.querySelectorAll("tbody tr")).map((tr) => {
    const cell = tr.querySelectorAll("td")[2];
    const code = cell.querySelector("[data-testid=leaderboard-code]")?.textContent ?? "";
    return (cell.textContent ?? "").slice(0, -code.length || undefined);
  });

describe("LeaderboardPage search", () => {
  it("searches the server as the player types, keeps the total, and says when nothing matches", async () => {
    const { container, getByTestId, findByText, getByLabelText } = renderPage();
    await waitFor(() => expect(names(container)).toEqual(["ann", "bob", "annie"]));
    expect(container.textContent).toContain("3 ranked players");
    const box = getByTestId("leaderboard-search");

    // Typed quickly: one search for the whole text, once the typing pauses.
    fireEvent.change(box, { target: { value: "a" } });
    fireEvent.change(box, { target: { value: "an" } });
    fireEvent.change(box, { target: { value: "ann " } });
    await waitFor(() => expect(names(container)).toEqual(["ann", "annie"]));
    expect(calls).toEqual([
      { after: undefined, search: undefined },
      { after: undefined, search: "ann" },
    ]);
    expect(container.textContent).toContain("3 ranked players");

    fireEvent.change(box, { target: { value: "zed" } });
    await findByText('No ranked player matches "zed".', undefined, { timeout: 3000 });

    // Clearing shows the whole board again from what was loaded, with no request.
    fireEvent.click(getByLabelText("Clear search"));
    await waitFor(() => expect(names(container)).toEqual(["ann", "bob", "annie"]));
    expect(calls.map((c) => c.search)).toEqual([undefined, "ann", "zed"]);
  });
});

describe("LeaderboardPage own row", () => {
  it("pins the player's own row under the list whenever their row in the list isn't on screen", async () => {
    account = { user: { uid: "u2" }, userData: { playKey: { connectCode: "BOB#2" } } };
    // The list's row seen or not, as the IntersectionObserver says.
    const observers: ((entries: { intersectionRatio: number }[]) => void)[] = [];
    vi.stubGlobal(
      "IntersectionObserver",
      class {
        constructor(cb: (entries: { intersectionRatio: number }[]) => void) {
          observers.push(cb);
        }
        observe() {}
        disconnect() {}
      },
    );
    try {
      const { container, queryByTestId } = renderPage();
      await waitFor(() => expect(names(container)).toContain("bob"));
      await waitFor(() => expect(queryByTestId("leaderboard-me")).not.toBeNull());
      const seen = (ratio: number) => act(() => observers[observers.length - 1]([{ intersectionRatio: ratio }]));

      seen(1);
      expect(queryByTestId("leaderboard-me")).toBeNull();
      // Scrolled away again: pinned again (it used to stay gone once the row had loaded).
      seen(0);
      expect(queryByTestId("leaderboard-me")).not.toBeNull();
      // Half in view isn't in view.
      seen(0.5);
      expect(queryByTestId("leaderboard-me")).not.toBeNull();
    } finally {
      vi.unstubAllGlobals();
      account = { user: null };
    }
  });
});
