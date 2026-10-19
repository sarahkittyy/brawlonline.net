import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { MemoryRouter, Route, Routes } from "react-router-dom";
import { afterEach, describe, expect, it, vi } from "vitest";

import { useRouteMemory } from "@/lib/hooks/use_route_memory";
import { HomePage } from "@/pages/home/home_page";

import { MainMenu } from "./main_menu";

vi.mock("@/pages/home/overview/overview", () => ({ HomeOverview: () => <div>overview page</div> }));
vi.mock("@/pages/home/spectate/spectate", () => ({ Spectate: () => <div>spectate page</div> }));
vi.mock("@/pages/home/mods/mods_tab", () => ({ ModsTab: () => <div>mods page</div> }));
vi.mock("@/pages/home/play_buttons/play_buttons", () => ({ PlayButtons: () => null }));

const Icon = () => null;
const menuItems = [
  { subpath: "home", title: () => "Home", Icon },
  { subpath: "replays", title: () => "Replays", Icon },
];

function renderApp(at: string) {
  render(
    <MemoryRouter initialEntries={[at]}>
      <Routes>
        <Route
          path="/main/*"
          element={
            <>
              <MainMenu menuItems={menuItems} />
              <Routes>
                <Route path="home/*" element={<HomePage />} />
                <Route path="replays/*" element={<div>replays page</div>} />
              </Routes>
            </>
          }
        />
      </Routes>
    </MemoryRouter>,
  );
}

const menuLink = (title: string) => screen.getByRole("link", { name: title });

describe("main menu", () => {
  afterEach(() => {
    cleanup();
    useRouteMemory.setState({ routeMemory: {} });
  });

  it.each(["spectate", "mods"])("Home while on Home > %s opens Overview", async (tab) => {
    useRouteMemory.setState({ routeMemory: { home: tab } });
    renderApp(`/main/home/${tab}`);
    expect(screen.getByText(`${tab} page`)).toBeTruthy();

    fireEvent.click(menuLink("Home"));

    expect(await screen.findByText("overview page")).toBeTruthy();
    expect(screen.queryByText(`${tab} page`)).toBeNull();
  });

  it("Home from another page opens the Home tab it was left on", async () => {
    useRouteMemory.setState({ routeMemory: { home: "spectate" } });
    renderApp("/main/replays");

    fireEvent.click(menuLink("Home"));

    expect(await screen.findByText("spectate page")).toBeTruthy();
    expect(useRouteMemory.getState().routeMemory["home"]).toBe("spectate");
  });
});
