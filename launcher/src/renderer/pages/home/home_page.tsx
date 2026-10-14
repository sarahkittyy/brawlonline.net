import styled from "@emotion/styled";
import React, { useCallback, useEffect } from "react";
import { Route, Routes, useNavigate, useParams } from "react-router-dom";

import { useRouteMemory } from "@/lib/hooks/use_route_memory";

import { HomePageMessages as Messages } from "./home_page.messages";
import { HOME_TABS } from "./home_routes";
import { HomeOverview } from "./overview/overview";
import { PlayButtons } from "./play_buttons/play_buttons";
import { Spectate } from "./spectate/spectate";
import { Tabs } from "./tabs/tabs";

const Outer = styled.div`
  display: flex;
  flex-flow: column;
  flex: 1;
  position: relative;
  min-width: 0;
`;

// The tabs, with Play unranked / Play ranked right after them.
const TabRow = styled.div`
  display: flex;
  align-items: flex-start;
  gap: 8px;
`;

const RestoreHomeScope = React.memo(function RestoreHomeScope() {
  const navigate = useNavigate();
  const lastTab = useRouteMemory((s) => s.routeMemory["home"]) || "overview";

  useEffect(() => {
    navigate(`/main/home/${lastTab}`, { replace: true });
  }, [lastTab, navigate]);

  return null;
});

export const HomePage = React.memo(function HomePage() {
  const params = useParams();
  const navigate = useNavigate();
  const setLastRoute = useRouteMemory((s) => s.setLastRoute);

  const splat = params["*"] || "";
  const firstSegment = splat.split("/")[0] || "";
  const currentTab = (HOME_TABS as readonly string[]).includes(firstSegment) ? firstSegment : undefined;

  const handleTabChange = useCallback(
    (tab: string) => {
      setLastRoute("home", tab);
      navigate(`/main/home/${tab}`);
    },
    [setLastRoute, navigate],
  );

  return (
    <Outer>
      <div style={{ flex: 1, display: "flex", flexDirection: "column", minHeight: 0 }}>
        <TabRow>
          <Tabs
            value={currentTab}
            highlightedTabIds={[]}
            onChange={handleTabChange}
            tabs={[
              { id: "overview", label: Messages.overview() },
              { id: "spectate", label: Messages.spectate() },
            ]}
          />
          <PlayButtons />
        </TabRow>
        <Routes>
          <Route index={true} element={<RestoreHomeScope />} />
          <Route path="overview" element={<HomeOverview />} />
          <Route path="spectate" element={<Spectate />} />
        </Routes>
      </div>
    </Outer>
  );
});
