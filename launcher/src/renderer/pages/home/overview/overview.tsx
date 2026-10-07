import React from "react";

import { AuthGuard } from "@/components/auth_guard";

import { MyRanking } from "./my_ranking/my_ranking";
import styles from "./overview.module.css";

// Slippi's overview also shows news, tournaments and the Ranked Day status. Those
// come from Slippi's services and are dropped (PPLUS_PORTING.md); the ranking stays.
export const HomeOverview = React.memo(function HomeOverview() {
  return (
    <div className={styles.container}>
      <div />
      <div />
      <div className={styles.rankedSidebar}>
        <AuthGuard render={() => <MyRanking />} />
      </div>
    </div>
  );
});
