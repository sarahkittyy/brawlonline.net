// Settings > Game > "Play Project+ with": Dolphin or EmissaryRecomp (docs/design/brawl-recompiled.md
// §4.2). It replaced "Play Button Action" (Launch Project+ / Launch Dolphin): Configure Dolphin in
// Settings > Netplay Dolphin opens Dolphin without a game. EmissaryRecomp's line says where it
// stands; picking it before it is set up opens Settings > Add-ons, and Play uses Dolphin until then.
import { useEffect } from "react";
import { useNavigate } from "react-router-dom";

import { refreshEmissary, useEmissary, watchEmissary } from "@/lib/emissary/use_emissary";
import { usePlayProjectPlusWith } from "@/lib/hooks/use_settings";

import { ADDONS_SETTINGS_PATH } from "../addons_settings/paths";
import { GameSettingsMessages as Messages } from "./game_settings.messages";
import { PlayProjectPlusWithView } from "./play_project_plus_with_view";

export const PlayProjectPlusWithSetting = ({ disabled }: { disabled?: boolean }) => {
  const [value, setValue] = usePlayProjectPlusWith();
  const emissary = useEmissary((s) => s.state);
  const navigate = useNavigate();
  useEffect(() => {
    watchEmissary();
    void refreshEmissary(false);
  }, []);
  return (
    <PlayProjectPlusWithView
      value={value}
      emissary={emissary}
      disabled={disabled}
      disabledReason={Messages.closeDolphinToChange()}
      onChange={(v) => void setValue(v)}
      onSetUp={() => navigate(ADDONS_SETTINGS_PATH)}
    />
  );
};
