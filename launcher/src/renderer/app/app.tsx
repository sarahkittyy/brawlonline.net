import React from "react";
import { Navigate, Route, Routes } from "react-router-dom";

import { Header } from "@/app/header/header";
import { LoginDialog } from "@/app/header/login_dialog";
import type { MenuItem } from "@/app/header/main_menu";
import { AuthGuard } from "@/components/auth_guard";
import { LoginNotice } from "@/components/login_notice/login_notice";
import { PersistentNotification } from "@/components/persistent_notification/persistent_notification";
import { RosettaInstallDialog } from "@/components/rosetta_install_dialog/rosetta_install_dialog";
import { SocialRoot } from "@/components/social/social_root";
import { installReplayVersionDialogPreview, ReplayVersionDialog } from "@/lib/pplus/replay_version_dialog";
import { refreshPPlusVersions } from "@/lib/pplus/use_pplus_versions";
import type { AuthUser } from "@/services/auth/types";

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
  }, []);
  return (
    <div
      style={{
        display: "flex",
        flexFlow: "column",
        height: "100%",
        width: "100%",
      }}
    >
      <div style={{ flexShrink: 0 }}>
        <Header menuItems={menuItems} />
      </div>
      <div style={{ flex: 1, overflow: "auto", display: "flex" }}>
        <Routes>
          {menuItems.map((item) => {
            const element = item.private ? (
              <AuthGuard fallback={<LoginNotice />} render={(user) => <item.Component user={user} />} />
            ) : (
              <item.Component />
            );
            return <Route key={item.subpath} path={`${item.subpath}/*`} element={element} />;
          })}
          {defaultRoute && <Route path="*" element={<Navigate replace={true} to={`${defaultRoute.subpath}`} />} />}
        </Routes>
      </div>
      <SocialRoot />
      <LoginDialog />
      <ReplayVersionDialog />
      <PersistentNotification />
      <RosettaInstallDialog />
    </div>
  );
});
