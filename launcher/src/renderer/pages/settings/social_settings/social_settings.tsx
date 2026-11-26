// Settings > Social: chat in the game (Disable Chat; Quick Chat's D-pad messages, docs/design/quick-chat.md); the
// player's Discord activity (renderer/lib/discord), and its two parts; their Twitch account
// (docs/design/twitch-live.md), connected or not, when logged in on a server that can connect one.
import { css } from "@emotion/react";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { Toggle } from "@/components/form/toggle";
import { TwitchConnection } from "@/components/twitch/twitch";
import { TwitchMessages } from "@/components/twitch/twitch.messages";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useDisableChat, useDiscordActivity, useDiscordButtons, useDiscordShowPlayers } from "@/lib/hooks/use_settings";
import { useTwitchLink } from "@/lib/twitch/use_twitch";

import { GameSettingsMessages } from "../game_settings/game_settings.messages";
import { SettingItem } from "../setting_item_section";
import { QuickChatSetting } from "./quick_chat_setting";
import { SocialSettingsMessages as Messages } from "./social_settings.messages";

/** Disable Chat, and Quick Chat's messages under it (dimmed while chat is off). */
export const ChatSettings = () => {
  const [disableChat, setDisableChat] = useDisableChat();
  const netplayDolphinOpen = useDolphinStore((store) => store.netplayOpened);
  return (
    <>
      <SettingItem>
        <Tooltip title={netplayDolphinOpen ? GameSettingsMessages.closeDolphinToChange() : ""} placement="bottom-start">
          <div>
            <Toggle
              value={disableChat}
              onChange={(checked) => void setDisableChat(checked)}
              label={Messages.disableChat()}
              description={Messages.disableChatDescription()}
              disabled={netplayDolphinOpen}
            />
          </div>
        </Tooltip>
      </SettingItem>
      <SettingItem name={Messages.quickChat()} description={Messages.quickChatDescription()}>
        <QuickChatSetting disabled={disableChat} />
      </SettingItem>
    </>
  );
};

export const SocialSettings = React.memo(function SocialSettings() {
  const [discordActivity, setDiscordActivity] = useDiscordActivity();
  const [showPlayers, setShowPlayers] = useDiscordShowPlayers();
  const [buttons, setButtons] = useDiscordButtons();
  const twitchAvailable = useTwitchLink()?.available === true;

  return (
    <div>
      <ChatSettings />
      <SettingItem>
        <Toggle
          value={discordActivity}
          onChange={(checked) => void setDiscordActivity(checked)}
          label={Messages.discordActivity()}
        />
        <div
          css={css`
            margin: 6px 0 0 4px;
            padding-left: 16px;
            border-left: 2px solid rgba(255, 255, 255, 0.12);
            display: flex;
            flex-direction: column;
            gap: 2px;
          `}
        >
          <Toggle
            value={showPlayers}
            onChange={(checked) => void setShowPlayers(checked)}
            label={Messages.discordShowPlayers()}
            disabled={!discordActivity}
          />
          <Toggle
            value={buttons}
            onChange={(checked) => void setButtons(checked)}
            label={Messages.discordButtons()}
            disabled={!discordActivity}
          />
        </div>
      </SettingItem>
      {twitchAvailable && (
        <SettingItem inline={true} name={TwitchMessages.twitch()}>
          <TwitchConnection />
        </SettingItem>
      )}
    </div>
  );
});
