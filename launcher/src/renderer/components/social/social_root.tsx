import React, { useEffect } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useHandOff } from "@/lib/social/use_hand_off";

import { ChatDock } from "./chat_dock";
import { InviteNotifications } from "./invite_notifications";
import { SocialMessages as Messages } from "./social.messages";

/**
 * The launcher's chat around the pages: the tab bar at the bottom (it takes its height from the
 * page) and the invite pop-ups. Answers to the player's own invites come here: an accepted Direct
 * invite sends this player's game to Direct 1v1 with the other player's code.
 */
export const SocialRoot = React.memo(function SocialRoot() {
  const loggedIn = useAccount((s) => s.user != null && s.userData?.playKey != null);
  const { showInfo, showWarning } = useToasts();
  const handOff = useHandOff();

  useEffect(() => {
    return window.electron.social.onEvent((event) => {
      if (event.type === "invite-answer") {
        const name = event.player.displayName;
        if (!event.accept) {
          showInfo(Messages.declined(name));
        } else if (event.kind === "room") {
          showInfo(Messages.acceptedRoom(name));
        } else {
          showInfo(Messages.acceptedDirect(name));
          void handOff(event.player.connectCode);
        }
      } else if (event.type === "replaced") {
        showWarning(Messages.replaced());
      }
    });
  }, [handOff, showInfo, showWarning]);

  if (!loggedIn) {
    return null;
  }
  return (
    <>
      <ChatDock />
      <InviteNotifications />
    </>
  );
});
