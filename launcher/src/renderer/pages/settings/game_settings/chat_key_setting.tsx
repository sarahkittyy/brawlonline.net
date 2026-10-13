import { chatKeyLabel, DEFAULT_CHAT_KEY } from "@common/chat_key";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import log from "electron-log";
import React from "react";

import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useServices } from "@/services";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";

/**
 * The key that opens the chat box in game (Dolphin's "Activate NetPlay Chat" hotkey, common/chat_key.ts).
 * Change waits for a key press; Esc cancels.
 */
export const ChatKeySetting = () => {
  const { dolphinService } = useServices();
  const netplayDolphinOpen = useDolphinStore((store) => store.netplayOpened);
  const [chatKey, setChatKey] = React.useState<string | null>(null);
  const [listening, setListening] = React.useState(false);
  const [refused, setRefused] = React.useState(false);

  React.useEffect(() => {
    // Read again when Dolphin closes: its Hotkey Settings may have changed the key.
    void dolphinService.getChatKey().then(setChatKey).catch(log.error);
  }, [dolphinService, netplayDolphinOpen]);

  React.useEffect(() => {
    if (!listening || netplayDolphinOpen) {
      setListening(false);
      return;
    }
    const onKeyDown = (event: KeyboardEvent) => {
      event.preventDefault();
      event.stopPropagation();
      if (event.code === "Escape") {
        setListening(false);
        return;
      }
      if (["Shift", "Control", "Alt", "Meta", "AltGraph"].includes(event.key)) {
        // Wait for the key itself (a key with a modifier held is refused below).
        return;
      }
      void dolphinService
        .setChatKey({
          code: event.code,
          key: event.key,
          ctrlKey: event.ctrlKey,
          altKey: event.altKey,
          shiftKey: event.shiftKey,
          metaKey: event.metaKey,
        })
        .then((newKey) => {
          if (newKey === null) {
            setRefused(true);
            return;
          }
          setChatKey(newKey);
          setListening(false);
        })
        .catch((err) => {
          log.error(err);
          setListening(false);
        });
    };
    // Capture: the press is the setting's, not a shortcut's (Ctrl +/- zoom and the like).
    window.addEventListener("keydown", onKeyDown, true);
    return () => window.removeEventListener("keydown", onKeyDown, true);
  }, [listening, netplayDolphinOpen, dolphinService]);

  const startListening = (event: React.MouseEvent<HTMLButtonElement>) => {
    // Unfocused, so the new key (Enter, Space) doesn't press the button again.
    event.currentTarget.blur();
    setRefused(false);
    setListening(true);
  };

  const reset = async () => {
    try {
      setChatKey(await dolphinService.setChatKey({ code: "KeyT", key: "t" }));
    } catch (err) {
      log.error(err);
    }
  };

  return (
    <SettingItem name={Messages.chatKey()} description={Messages.chatKeyDescription()}>
      <Tooltip title={netplayDolphinOpen ? Messages.closeDolphinToChange() : ""} placement="bottom-start">
        <div
          css={css`
            display: flex;
            align-items: center;
            gap: 10px;
          `}
        >
          <Button
            variant="outlined"
            color="secondary"
            disabled={netplayDolphinOpen || chatKey === null}
            onClick={listening ? () => setListening(false) : startListening}
            css={css`
              min-width: 140px;
              text-transform: none;
            `}
          >
            {listening ? Messages.chatKeyPressAKey() : chatKey !== null ? chatKeyLabel(chatKey) : "…"}
          </Button>
          {!listening && chatKey !== null && chatKeyLabel(chatKey) !== chatKeyLabel(DEFAULT_CHAT_KEY) && (
            <Button color="secondary" disabled={netplayDolphinOpen} onClick={reset}>
              {Messages.chatKeyReset()}
            </Button>
          )}
          {listening && (
            <Typography variant="caption">
              {refused ? Messages.chatKeyRefused() : Messages.chatKeyEscToCancel()}
            </Typography>
          )}
        </div>
      </Tooltip>
    </SettingItem>
  );
};
