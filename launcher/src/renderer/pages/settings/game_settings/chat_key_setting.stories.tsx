import type { ChatKeyPress } from "@common/chat_key";
import { chatKeyExpression, DEFAULT_CHAT_KEY } from "@common/chat_key";

import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";

import { ChatKeySetting } from "./chat_key_setting";

export default {
  title: "pages/Settings/ChatKeySetting",
  argTypes: {},
};

const provider = (platform: string, initial: string) => {
  let chatKey = initial;
  const dolphinService = {
    getChatKey: async () => chatKey,
    setChatKey: async (press: ChatKeyPress) => {
      const expression = chatKeyExpression(press, platform);
      if (expression !== null) {
        chatKey = expression;
      }
      return expression;
    },
  };
  return createServiceProvider({ services: { dolphinService } as unknown as Services }).ServiceProvider;
};

const WindowsProvider = provider("win32", DEFAULT_CHAT_KEY);
const LinuxProvider = provider("linux", "`slash`");

/** Click the key, then press another (Esc cancels; Ctrl+T is refused). */
export const Windows = () => {
  useDolphinStore.setState({ netplayOpened: false });
  return (
    <WindowsProvider>
      <ChatKeySetting />
    </WindowsProvider>
  );
};

/** Set to / on Linux (XInput2 names it "slash"): Reset to T shows. */
export const LinuxSlash = () => {
  useDolphinStore.setState({ netplayOpened: false });
  return (
    <LinuxProvider>
      <ChatKeySetting />
    </LinuxProvider>
  );
};

export const DolphinOpen = () => {
  useDolphinStore.setState({ netplayOpened: true });
  return (
    <WindowsProvider>
      <ChatKeySetting />
    </WindowsProvider>
  );
};
