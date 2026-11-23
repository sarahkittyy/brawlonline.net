import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, fireEvent, render, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { useAccount } from "@/lib/hooks/use_account";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { usePlayerMenu } from "@/lib/social/use_player_menu";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { PlayerInfo } from "./player_info";

// Accounts by their current code; OLD#1 is a code nobody has now.
const profiles: Record<string, { uid: string; displayName: string; connectCode: string }> = {
  "ZH#403": { uid: "zhime", displayName: "Zhime", connectCode: "ZH#403" },
};

function show(connectCode: string | undefined) {
  const fetchProfile = vi.fn(async (id: string) => {
    const p = profiles[id];
    if (!p) {
      throw new Error("not found");
    }
    return p;
  });
  const { ServiceProvider } = createServiceProvider({
    services: { backendService: { fetchProfile } } as unknown as Services,
  });
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
  const queryClient = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const view = render(
    <QueryClientProvider client={queryClient}>
      <ServiceProvider>
        <PlayerInfo isTeams={false} playerIndex={0} connectCode={connectCode} displayName="Zhime" />
      </ServiceProvider>
    </QueryClientProvider>,
  );
  return { view, fetchProfile, queryClient };
}

describe("a replay's player name", () => {
  afterEach(() => {
    cleanup();
    useProfileView.setState({ target: null });
    usePlayerMenu.setState({ request: null });
  });

  it("opens the profile of the account that has the replay's code, and its right-click menu", async () => {
    const { view, queryClient } = show("ZH#403");
    await waitFor(() => expect(view.container.querySelector("[data-profile-link=zhime]")).not.toBeNull());
    // The hover card and the dialog find the profile already fetched.
    expect(queryClient.getQueryData(["profile", "zhime"])).toEqual(profiles["ZH#403"]);
    fireEvent.click(view.container.querySelector("[data-profile-link=zhime]")!);
    expect(useProfileView.getState().target).toEqual({ uid: "zhime", displayName: "Zhime", connectCode: "ZH#403" });
    fireEvent.contextMenu(view.getByText("Zhime"), { clientX: 5, clientY: 6 });
    expect(usePlayerMenu.getState().request?.player).toMatchObject({ uid: "zhime", connectCode: "ZH#403" });
  });

  it("stays plain text without a code, or for a code no account has now", async () => {
    const none = show(undefined);
    expect(none.fetchProfile).not.toHaveBeenCalled();
    expect(none.view.container.querySelector("[data-profile-link]")).toBeNull();
    cleanup();
    const gone = show("OLD#1");
    await waitFor(() => expect(gone.fetchProfile).toHaveBeenCalledWith("OLD#1"));
    expect(gone.view.container.querySelector("[data-profile-link]")).toBeNull();
    expect(gone.view.getByText("Zhime")).toBeTruthy();
  });
});
