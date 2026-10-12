import React from "react";

import { InterfaceZoom } from "./interface_zoom/interface_zoom";
import { LanguageSelector } from "./language_selector/language_selector";
import { NotificationVolume } from "./notification_volume/notification_volume";
import { RankDisplayToggle } from "./rank_display_toggle/rank_display_toggle";

export const AppearanceSettings = React.memo(() => {
  return (
    <div>
      <LanguageSelector />
      <InterfaceZoom />
      <RankDisplayToggle />
      <NotificationVolume />
    </div>
  );
});
