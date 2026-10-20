import { AdvancedAppSettings } from "./advanced_app_settings/advanced_app_settings";
import { AppearanceSettings } from "./appearance_settings/appearance_settings";
import { ClipsSettings } from "./clips_settings/clips_settings";
import { SettingsCreateMessages as Messages } from "./create.messages";
import { CreditsPage } from "./credits_page/credits_page";
import { NetplayDolphinSettings } from "./dolphin_settings/netplay_dolphin_settings";
import { PLAYBACK_DOLPHIN_SETTINGS_PATH } from "./dolphin_settings/paths";
import { PlaybackDolphinSettings } from "./dolphin_settings/playback_dolphin_settings";
import { GameSettings } from "./game_settings/game_settings";
import { HelpPage } from "./help_page/help_page";
import { ReplaySettings } from "./replay_settings/replay_settings";
import { SettingsPage } from "./settings_page";
import type { SettingSection } from "./types";

export function createSettingsPage(): { Page: React.ComponentType } {
  const settings: SettingSection[] = [
    {
      title: () => Messages.generalSettings(),
      items: [
        {
          name: () => Messages.game(),
          path: "game-options",
          component: <GameSettings />,
        },
        {
          name: () => Messages.replays(),
          path: "replay-options",
          component: <ReplaySettings />,
        },
        {
          name: () => Messages.clips(),
          path: "clips-options",
          component: <ClipsSettings />,
        },
      ],
    },
    {
      title: () => Messages.dolphinSettings(),
      items: [
        {
          name: () => Messages.netplay(),
          path: "netplay-dolphin-settings",
          component: <NetplayDolphinSettings />,
        },
        {
          name: () => Messages.playback(),
          path: PLAYBACK_DOLPHIN_SETTINGS_PATH.replace("/settings/", ""),
          component: <PlaybackDolphinSettings />,
        },
      ],
    },
    {
      title: () => Messages.appSettings(),
      items: [
        {
          name: () => Messages.appearance(),
          path: "appearance-settings",
          component: <AppearanceSettings />,
        },
        {
          name: () => Messages.advanced(),
          path: "advanced-settings",
          component: <AdvancedAppSettings />,
        },
      ],
    },
    {
      items: [
        {
          name: () => Messages.help(),
          path: "help",
          component: <HelpPage />,
        },
        {
          name: () => Messages.credits(),
          path: "credits",
          component: <CreditsPage />,
        },
      ],
    },
  ];

  const Page = () => <SettingsPage settings={settings} />;

  return { Page };
}
