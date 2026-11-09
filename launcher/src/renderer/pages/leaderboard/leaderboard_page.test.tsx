import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, waitFor } from "@testing-library/react";
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

vi.mock("@/services", () => ({ useServices: () => ({ backendService: { fetchLeaderboard } }) }));
vi.mock("@/lib/hooks/use_account", () => ({
  useAccount: (select: (s: { user: null }) => unknown) => select({ user: null }),
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
  Array.from(container.querySelectorAll("tbody tr")).map((tr) => tr.querySelectorAll("td")[2].textContent);

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
