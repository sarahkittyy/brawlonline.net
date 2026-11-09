import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { act, cleanup, fireEvent, render, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { UserName } from "@/components/user_name/user_name";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import type { PlayerProfile, ProfileUpdate } from "@/services/backend/types";
import { useGameAssets } from "@/styles/game_theme";

import { ProfileDialog } from "./profile_dialog";

const base: PlayerProfile = {
  uid: "me",
  displayName: "Sarah",
  connectCode: "SARA#64",
  link: "https://brawlonline.net/profile/sara-64",
  ranked: {
    rank: { tier: "gold", division: 2 },
    rating: 1523.4,
    setsPlayed: 54,
    setsToPlace: 0,
    wins: 34,
    losses: 20,
    position: 4,
    rankedPlayers: 6,
  },
  mains: [],
  country: null,
  socials: [],
};

let stored: PlayerProfile = base;
const fetchProfile = vi.fn(async (id: string) => (id === "me" ? stored : { ...base, uid: id, displayName: "other" }));
const updateProfile = vi.fn(async (u: ProfileUpdate) => {
  stored = {
    ...stored,
    ...(u.mains && { mains: u.mains }),
    ...(u.country !== undefined && { country: u.country }),
    ...(u.socials && {
      socials: Object.entries(u.socials)
        .filter(([, v]) => v)
        .map(([platform, v]) => ({ platform: platform as "twitch", handle: v!, display: `@${v!}`, url: null })),
    }),
  };
  return stored;
});
const changeDisplayName = vi.fn(async (name: string) => {
  stored = { ...stored, displayName: name };
});
vi.mock("@/services", () => ({
  useServices: () => ({ backendService: { fetchProfile, updateProfile, changeDisplayName } }),
}));

const setDisplayName = vi.fn();
const account = { user: { uid: "me" }, userData: { playKey: { connectCode: "SARA#64" } }, setDisplayName };
vi.mock("@/lib/hooks/use_account", () => ({
  useAccount: (select: (s: typeof account) => unknown) => select(account),
}));

const copied: string[] = [];
vi.mock("@/lib/hooks/use_toasts", () => ({
  useToasts: () => ({ showSuccess: () => undefined, showError: () => undefined, showInfo: () => undefined }),
}));

const openExternal = vi.fn(async () => undefined);
Object.assign(window, { electron: { social: { setIgnored: vi.fn() }, shell: { openExternal } } });
Object.assign(navigator, { clipboard: { writeText: async (t: string) => void copied.push(t) } });

function open(uid = "me") {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const view = render(
    <QueryClientProvider client={client}>
      <ProfileDialog />
    </QueryClientProvider>,
  );
  act(() => useProfileView.getState().open({ uid }));
  return view;
}

/** Stock icons as the extracted assets have them (the player's disc is set up). */
function withStocks(keys: string[]) {
  const stocks = Object.fromEntries(keys.map((k) => [k, { file: `stocks/${k}.png`, origin: "sd" }]));
  useGameAssets.setState({ state: { status: "READY" as any, manifest: { stocks } as any } });
}

describe("profile edit mode", () => {
  beforeEach(() => {
    stored = base;
    vi.clearAllMocks();
    copied.length = 0;
    useGameAssets.setState({ state: null });
  });
  afterEach(() => {
    cleanup();
    useProfileView.setState({ target: null });
  });

  it("hides mains, country and socials until they are set, and only the own profile has the pencil", async () => {
    const view = open("them");
    await view.findByTestId("profile-view");
    expect(view.queryByTestId("profile-edit")).toBeNull();
    cleanup();
    const own = open();
    await own.findByTestId("profile-view");
    expect(own.queryByTestId("profile-mains")).toBeNull();
    expect(own.queryByTestId("profile-country")).toBeNull();
    expect(own.queryByTestId("profile-socials")).toBeNull();
    expect(own.getByTestId("profile-edit")).toBeTruthy();
  });

  it("renames through the display name path when the name box loses focus", async () => {
    const view = open();
    fireEvent.click(await view.findByTestId("profile-edit"));
    const box = view.getByTestId("profile-name-input") as HTMLInputElement;
    fireEvent.change(box, { target: { value: "  Kitty " } });
    fireEvent.blur(box);
    await waitFor(() => expect(changeDisplayName).toHaveBeenCalledWith("Kitty"));
    expect(setDisplayName).toHaveBeenCalledWith("Kitty");
    // Unchanged or invalid: no call.
    fireEvent.change(box, { target: { value: "this name is far too long" } });
    fireEvent.blur(box);
    expect(changeDisplayName).toHaveBeenCalledTimes(1);
    expect(view.getByText("Display name is too long")).toBeTruthy();
  });

  it("picks mains with a primary: click toggles, double-click makes primary, Ok saves", async () => {
    withStocks(["marth", "sheik", "fox", "random"]);
    const view = open();
    fireEvent.click(await view.findByTestId("profile-edit"));
    fireEvent.click(view.getByLabelText("Change mains"));
    const picker = await view.findByTestId("mains-picker");
    const pick = (k: string) => picker.querySelector(`[data-testid=pick-${k}]`) as HTMLButtonElement;
    fireEvent.click(pick("sheik"));
    fireEvent.click(pick("marth"));
    fireEvent.click(pick("fox"));
    fireEvent.click(pick("fox"));
    // Sheik, picked first, is primary until Marth is double-clicked.
    expect(pick("sheik").className).toMatch(/primary/);
    fireEvent.doubleClick(pick("marth"));
    expect(pick("marth").className).toMatch(/primary/);
    expect(pick("fox").getAttribute("aria-pressed")).toBe("false");
    fireEvent.click(view.getByText("Ok"));
    await waitFor(() => expect(updateProfile).toHaveBeenCalledWith({ mains: ["marth", "sheik"] }));
    // Shown in the profile, primary first, as stock icons.
    const mains = await view.findByTestId("profile-mains");
    await waitFor(() =>
      expect(Array.from(mains.querySelectorAll("img")).map((i) => i.getAttribute("alt"))).toEqual(["Marth", "Sheik"]),
    );
  });

  it('shows the country under the code, Random as a "?", and saves each "Show next to name"', async () => {
    stored = { ...base, mains: ["random", "fox"], country: "us", showCountry: true, showMain: true };
    withStocks(["fox"]);
    const view = open();
    const country = await view.findByTestId("profile-country");
    expect(country.textContent).toBe("United States");
    expect(country.previousElementSibling?.textContent).toBe("SARA#64");
    expect(view.getByTestId("stock-random").querySelector("text")?.textContent).toBe("?");
    fireEvent.click(view.getByTestId("profile-edit"));
    fireEvent.click(view.getByTestId("show-main"));
    await waitFor(() => expect(updateProfile).toHaveBeenCalledWith({ showMain: false }));
    fireEvent.click(view.getByTestId("show-country"));
    await waitFor(() => expect(updateProfile).toHaveBeenCalledWith({ showCountry: false }));
  });

  it("shows a small profile card when a name is hovered", async () => {
    stored = { ...base, country: "se", mains: ["fox"] };
    const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
    const view = render(
      <QueryClientProvider client={client}>
        <UserName user={{ uid: "me", displayName: "Sarah" }} />
      </QueryClientProvider>,
    );
    fireEvent.mouseOver(view.getByText("Sarah"));
    const card = await view.findByTestId("mini-profile", undefined, { timeout: 3000 });
    expect(card.textContent).toContain("SARA#64");
    expect(card.textContent).toContain("Sweden");
    expect(card.textContent).toContain("Gold 2");
    expect(fetchProfile).toHaveBeenCalledWith("me");
  });

  it("shows mains as name tags without a disc", async () => {
    stored = { ...base, mains: ["mr_game_and_watch", "random"] };
    const view = open();
    const mains = await view.findByTestId("profile-mains");
    expect(mains.querySelector("img")).toBeNull();
    expect(mains.textContent).toBe("Mains:Mr. Game & WatchRandom");
  });

  it("picks a country from the searchable flag grid, and removes it", async () => {
    const view = open();
    fireEvent.click(await view.findByTestId("profile-edit"));
    fireEvent.click(view.getByLabelText("Change country"));
    const picker = await view.findByTestId("country-picker");
    fireEvent.change(picker.querySelector("input")!, { target: { value: "swed" } });
    expect(picker.querySelectorAll("[data-testid^=pick-country-]").length).toBe(1);
    fireEvent.click(picker.querySelector("[data-testid=pick-country-se]")!);
    await waitFor(() => expect(updateProfile).toHaveBeenCalledWith({ country: "se" }));
    await waitFor(() => expect(view.getByTestId("profile-country").textContent).toContain("Sweden"));
    fireEvent.click(view.getByLabelText("Change country"));
    fireEvent.click((await view.findByTestId("country-picker")).querySelector("[data-testid=pick-no-country]")!);
    await waitFor(() => expect(updateProfile).toHaveBeenLastCalledWith({ country: null }));
  });

  it("saves a social account when its box loses focus; the profile shows its logo", async () => {
    const view = open();
    fireEvent.click(await view.findByTestId("profile-edit"));
    const twitch = view.getByTestId("social-input-twitch");
    fireEvent.change(twitch, { target: { value: "twitch.tv/sarahkitty" } });
    fireEvent.blur(twitch);
    await waitFor(() => expect(updateProfile).toHaveBeenCalledWith({ socials: { twitch: "twitch.tv/sarahkitty" } }));
    // Leaving edit mode: the row of logos; Discord-like accounts without a page copy the name.
    fireEvent.click(view.getByTestId("profile-edit"));
    const row = await view.findByTestId("profile-socials");
    const button = row.querySelector("button")!;
    expect(button.getAttribute("aria-label")).toBe("Twitch: @twitch.tv/sarahkitty");
    fireEvent.click(button);
    await waitFor(() => expect(copied).toEqual(["twitch.tv/sarahkitty"]));
  });
});
