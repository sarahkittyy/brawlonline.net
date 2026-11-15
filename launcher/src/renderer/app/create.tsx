import AlbumIcon from "@mui/icons-material/Album";
import HistoryIcon from "@mui/icons-material/History";
import LeaderboardOutlinedIcon from "@mui/icons-material/LeaderboardOutlined";
import LiveTvIcon from "@mui/icons-material/LiveTv";
import RuleOutlinedIcon from "@mui/icons-material/RuleOutlined";
import SlowMotionVideoIcon from "@mui/icons-material/SlowMotionVideo";
import SportsEsportsOutlinedIcon from "@mui/icons-material/SportsEsportsOutlined";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import React from "react";
import { HashRouter as Router, Navigate, Route, Routes } from "react-router-dom";

import { ThemeColorPicker } from "@/components/theme_color_picker/theme_color_picker";
import { useAppStore } from "@/lib/hooks/use_app_store";
import { useLastPageTracker } from "@/lib/hooks/use_last_page";
import { usePageRequestListeners } from "@/lib/hooks/use_page_request_listeners";
import { usePageNavigationShortcuts } from "@/lib/hooks/use_shortcuts";
import { useBrawlLinks } from "@/lib/rulesets/use_brawl_links";
import { ModsTab } from "@/pages/home/mods/mods_tab";
import { Spectate } from "@/pages/home/spectate/spectate";
import { LeaderboardPage } from "@/pages/leaderboard/leaderboard_page";
import { MatchHistoryPage } from "@/pages/match_history/match_history_page";
import { NotFoundPage } from "@/pages/not_found/not_found_page";
import { PlayPage } from "@/pages/play/play_page";
import { createQuickStartPage } from "@/pages/quick_start/create";
import { useQuickStartStore } from "@/pages/quick_start/use_quick_start";
import { createReplaysPage } from "@/pages/replays/create";
import { useHighlightNavigation } from "@/pages/replays/replay_file_stats/highlights/highlight_playlist";
import { RulesetsPage } from "@/pages/rulesets/rulesets_page";
import { createSettingsPage } from "@/pages/settings/create";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import type { MainMenuItem } from "./app";
import { App as AppImpl } from "./app";
import { CreateAppMessages as Messages } from "./create.messages";
import { OtherGamesBadge, SpectateBadge } from "./shell/nav_badges";
import { ShellMessages } from "./shell/shell.messages";

export function createApp({ services }: { services: Services }): {
  App: React.ComponentType;
} {
  const { Page: SettingsPage } = createSettingsPage();
  const { Page: QuickStartPage } = createQuickStartPage();
  const { Page: ReplaysPage } = createReplaysPage();

  // docs/design/launcher-redesign.md §2: the sidebar's pages, in the order they're used.
  const menuItems: MainMenuItem[] = [
    {
      subpath: "play",
      title: () => ShellMessages.play(),
      Component: PlayPage,
      Icon: SportsEsportsOutlinedIcon,
      default: true,
    },
    {
      subpath: "other-games",
      title: () => ShellMessages.otherGames(),
      Component: ModsTab,
      Icon: AlbumIcon,
      Badge: OtherGamesBadge,
    },
    {
      subpath: "spectate",
      title: () => ShellMessages.spectate(),
      Component: Spectate,
      Icon: LiveTvIcon,
      Badge: SpectateBadge,
    },
    {
      subpath: "matches",
      title: () => Messages.matchHistory(),
      Component: MatchHistoryPage,
      Icon: HistoryIcon,
      private: true,
    },
    {
      subpath: "replays",
      title: () => Messages.replays(),
      Component: ReplaysPage,
      Icon: SlowMotionVideoIcon,
    },
    {
      subpath: "leaderboard",
      title: () => Messages.leaderboard(),
      Component: LeaderboardPage,
      Icon: LeaderboardOutlinedIcon,
    },
    {
      subpath: "rulesets",
      title: () => Messages.rulesets(),
      Component: RulesetsPage,
      Icon: RuleOutlinedIcon,
    },
  ];

  const navigationPaths = menuItems.filter((item) => !item.hidden).map((item) => item.subpath);

  const MainAppPage = React.memo(() => {
    usePageNavigationShortcuts(navigationPaths);
    // `brawlonline://room/create?ruleset=...` links (the website's /r/<code> pages).
    useBrawlLinks();

    return <AppImpl menuItems={menuItems} />;
  });

  // Smart root redirect that checks if quick start is needed
  const RootRedirect = () => {
    const steps = useQuickStartStore((store) => store.steps);

    // Only redirect to the landing page if we actually have quick start steps
    if (steps.length > 0) {
      return <Navigate to="/landing" replace={true} />;
    }

    // Otherwise, go to the main page
    return <Navigate to="/main" replace={true} />;
  };

  const AppRoutes = () => {
    const currentLanguage = useAppStore((state) => state.currentLanguage);

    // Track last non-settings URL for modal return navigation
    useLastPageTracker();

    // Then add the page request listeners
    usePageRequestListeners();

    // The playback Dolphin's highlight list buttons and stars (highlight_playlist.ts), with any page open.
    useHighlightNavigation();

    return (
      <>
        <Routes key={currentLanguage}>
          <Route path="/main/*" element={<MainAppPage />} />
          <Route path="/landing" element={<QuickStartPage />} />
          <Route path="/settings/*" element={<SettingsPage />} />
          <Route path="/" element={<RootRedirect />} />
          <Route element={<NotFoundPage />} />
        </Routes>
        {/* Settings > Appearance > Theme Color's wheel, over the main page. */}
        <ThemeColorPicker />
      </>
    );
  };

  const { ServiceProvider } = createServiceProvider({ services });

  const queryClient = new QueryClient({
    defaultOptions: {
      queries: {
        refetchIntervalInBackground: false,
        refetchOnMount: false,
        refetchOnWindowFocus: false,
        refetchOnReconnect: false,
        refetchInterval: false,
        retry: false,
      },
    },
  });

  // Providers need to be initialized before the rest of the app can use them
  const withProviders = (Component: React.ComponentType) => {
    return () => (
      <QueryClientProvider client={queryClient}>
        <ServiceProvider>
          <Router
            future={{
              v7_startTransition: true,
            }}
          >
            <Component />
          </Router>
        </ServiceProvider>
      </QueryClientProvider>
    );
  };

  return { App: withProviders(AppRoutes) };
}
