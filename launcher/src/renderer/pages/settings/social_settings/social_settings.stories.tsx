import "./social_settings.stories.setup";

import { quickChatIndex } from "@common/quick_chat";
import React from "react";

import { useSettingsStore } from "@/lib/hooks/use_settings";

import { ChatSettings } from "./social_settings";

export default {
  title: "pages/Settings/SocialSettings",
  argTypes: {},
};

// Sample messages (story data): short ones, one at chat's 100 characters, kana.
const SAMPLE = (() => {
  const m = Array<string>(16).fill("");
  m[quickChatIndex("up", "up")] = "ggs!";
  m[quickChatIndex("up", "right")] = "one more?";
  m[quickChatIndex("up", "down")] = "that was close, well played";
  m[quickChatIndex("up", "left")] = "brb";
  m[quickChatIndex("right", "up")] = "glhf";
  m[quickChatIndex("right", "right")] = "ready when you are";
  m[quickChatIndex("down", "down")] = "lag?";
  m[quickChatIndex("left", "left")] = "ありがとう！";
  m[quickChatIndex("left", "right")] =
    "changing my controller real quick, give me one minute and I'll be right back to keep playing ok";
  return m;
})();

const WithSettings = ({ quickChat, disableChat }: { quickChat: string[]; disableChat: boolean }) => {
  React.useState(() => {
    useSettingsStore.getState().applyUpdates([
      { key: "quickChat", value: quickChat },
      { key: "disableChat", value: disableChat },
    ] as never);
    return null;
  });
  return (
    <div style={{ maxWidth: 720, padding: 24 }}>
      <ChatSettings />
    </div>
  );
};

/** Messages set: the up group open, its four around the D-pad; the dots show each group's. */
export const QuickChat = () => <WithSettings quickChat={SAMPLE} disableChat={false} />;

/** Nothing set yet (the default). */
export const QuickChatEmpty = () => <WithSettings quickChat={Array(16).fill("")} disableChat={false} />;

/** Disable Chat on: Quick Chat dimmed. */
export const ChatDisabled = () => <WithSettings quickChat={SAMPLE} disableChat={true} />;

/** The left group: a message at chat's 100 characters beside the D-pad. */
export const QuickChatLongMessage = {
  render: () => <WithSettings quickChat={SAMPLE} disableChat={false} />,
  play: async () => {
    await new Promise((resolve) => setTimeout(resolve, 200));
    document
      .querySelector<HTMLButtonElement>('button[role="radio"][aria-label="←"]')
      ?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
  },
};
