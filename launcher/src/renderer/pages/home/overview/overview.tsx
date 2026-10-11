import React from "react";

import { AuthGuard } from "@/components/auth_guard";

import { MyRanking } from "./my_ranking/my_ranking";
import { OnlinePlayers } from "./online_players/online_players";
import styles from "./overview.module.css";
import { PublicRooms } from "./public_rooms/public_rooms";

// Slippi's overview shows news and tournaments in its two wide columns. Those come from Slippi's
// services and are dropped (PPLUS_PORTING.md); the two columns hold the players online and the
// public rooms (docs/design/rooms.md §1.1), and the ranking stays, with the players online in the
// launcher under it (docs/social-protocol.md).
export const HomeOverview = React.memo(function HomeOverview() {
  return (
    <div className={styles.container}>
      <PublicRooms />
      <div className={styles.rankedSidebar}>
        <AuthGuard render={() => <MyRanking />} />
        <OnlinePlayers />
      </div>
    </div>
  );
});
