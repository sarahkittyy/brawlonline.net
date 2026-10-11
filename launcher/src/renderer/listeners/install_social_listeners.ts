import { useAccount } from "@/lib/hooks/use_account";
import { playSound } from "@/lib/social/sounds";
import { useSocial } from "@/lib/social/use_social";

/**
 * The launcher's social connection follows the logged-in account (once online play is set up);
 * a message opens its player's tab (collapsed, flashing) and, with the bell on, a message or an
 * invite plays its sound. Invite answers and toasts are handled by SocialRoot, which has the
 * services' hooks.
 */
export function installSocialListeners() {
  const social = window.electron.social;
  social.onStateChange((state) => useSocial.getState().setState(state));
  void social.getState().then((state) => useSocial.getState().setState(state));

  let connected: string | undefined;
  const follow = () => {
    const { user, userData } = useAccount.getState();
    const uid = user && userData?.playKey ? user.uid : undefined;
    if (uid === connected) {
      return;
    }
    connected = uid;
    void (uid ? social.connect(uid) : social.disconnect());
  };
  useAccount.subscribe(follow);
  follow();

  social.onEvent((event) => {
    switch (event.type) {
      case "message": {
        const tab = useSocial.getState().tabs.find((t) => t.uid === event.uid);
        useSocial.getState().openTab(event.uid, false);
        if (!tab?.open || !document.hasFocus()) {
          playSound("chat-message");
        }
        break;
      }
      case "invite":
        playSound("invite");
        break;
    }
  });
}
