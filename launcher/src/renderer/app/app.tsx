import React from "react";
import { Navigate, Route, Routes } from "react-router-dom";

import { LoginDialog } from "@/app/header/login_dialog";
import { PlayersRail } from "@/app/shell/players_rail";
import type { NavItem } from "@/app/shell/sidebar";
import { Sidebar } from "@/app/shell/sidebar";
import { AuthGuard } from "@/components/auth_guard";
import { LoginNotice } from "@/components/login_notice/login_notice";
import { RosettaInstallDialog } from "@/components/rosetta_install_dialog/rosetta_install_dialog";
import { SocialRoot } from "@/components/social/social_root";
import { installPPlusBuildDialogPreview, PPlusBuildDialog } from "@/lib/pplus/pplus_build_dialog";
import { installReplayVersionDialogPreview, ReplayVersionDialog } from "@/lib/pplus/replay_version_dialog";
import { refreshPPlusVersions } from "@/lib/pplus/use_pplus_versions";
import { ProjectPunchDialog } from "@/lib/project_punch/project_punch_dialog";
import { ProfileNavigator } from "@/pages/profile/profile_page";
import type { AuthUser } from "@/services/auth/types";

type MenuItem = NavItem & {
  /** A page with no sidebar entry (reached from elsewhere: a profile, the old match history address). */
  hidden?: boolean;
};

type PrivateMenuItem = MenuItem & {
  Component: React.ComponentType<{ user: AuthUser }>;
  default?: boolean;
  private: true;
};

export type MainMenuItem =
  | (MenuItem & {
      Component: React.ComponentType;
      default?: boolean;
      private?: false;
    })
  | PrivateMenuItem;

export const App = React.memo(({ menuItems }: { menuItems: readonly MainMenuItem[] }) => {
  const defaultRoute = menuItems.find((item) => item.default);
  // Which Project+ releases' replays can play here (the replay rows' version tags).
  React.useEffect(() => {
    void refreshPPlusVersions();
    installReplayVersionDialogPreview();
    installPPlusBuildDialogPreview();
  }, []);
  // docs/design/launcher-redesign.md §2: the sidebar, the page, the players rail.
  return (
    <div
      style={{
        display: "flex",
        height: "100%",
        width: "100%",
      }}
    >
      <Sidebar items={menuItems.filter((item) => !item.hidden)} />
      <main style={{ flex: 1, minWidth: 0, overflow: "auto", display: "flex", backgroundColor: "var(--page)" }}>
        <Routes>
          {menuItems.map((item) => {
            const element = item.private ? (
              <AuthGuard fallback={<LoginNotice />} render={(user) => <item.Component user={user} />} />
            ) : (
              <item.Component />
            );
            return <Route key={item.subpath} path={`${item.subpath}/*`} element={element} />;
          })}
          {/* The Home tabs' old addresses (route memory, links, page requests). */}
          <Route path="home/spectate/*" element={<Navigate replace={true} to="/main/spectate" />} />
          <Route path="home/mods/*" element={<Navigate replace={true} to="/main/other-games" />} />
          <Route path="home/*" element={<Navigate replace={true} to="/main/play" />} />
          {defaultRoute && <Route path="*" element={<Navigate replace={true} to={`${defaultRoute.subpath}`} />} />}
        </Routes>
      </main>
      <PlayersRail />
      <SocialRoot />
      <ProfileNavigator />
      <LoginDialog />
      <ReplayVersionDialog />
      <PPlusBuildDialog />
      <ProjectPunchDialog />
      <RosettaInstallDialog />
    </div>
  );
});
