import { PRODUCT_NAME } from "@common/product";
import { IsoValidity } from "@common/types";
import { useCallback } from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { HeaderMessages as Messages } from "@/app/header/header.messages";
import { useAccount } from "@/lib/hooks/use_account";
import { useAppStore } from "@/lib/hooks/use_app_store";
import { useIsoVerification } from "@/lib/hooks/use_iso_verification";
import { useSettings } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

import { useDolphinActions } from "./use_dolphin_actions";

/** The two dialogs Play can open (the header renders them), so Play works from anywhere. */
export const usePlayDialogs = create(
  combine({ startGameOpen: false, activateOnlineOpen: false }, (set) => ({
    setStartGameOpen: (open: boolean) => set({ startGameOpen: open }),
    setActivateOnlineOpen: (open: boolean) => set({ activateOnlineOpen: open }),
  })),
);

/**
 * The Play button's checks and launch (Slippi's `onPlay`): logged in and online, a play key on
 * disk, a valid disc image, then the netplay Dolphin. Resolves to whether the game was started;
 * when a check fails it shows its dialog or error, as the Play button does.
 */
export const usePlay = () => {
  const { dolphinService, backendService } = useServices();
  const currentUser = useAccount((store) => store.user);
  const userData = useAccount((store) => store.userData);
  const serverError = useAccount((store) => store.serverError);
  const isoPath = useSettings((store) => store.settings.isoPath) || undefined;
  const isOnline = useAppStore((state) => state.isOnline);
  const { showError } = useToasts();
  const { launchNetplay } = useDolphinActions(dolphinService);
  const { setStartGameOpen, setActivateOnlineOpen } = usePlayDialogs();

  return useCallback(
    async (offlineOnly?: boolean): Promise<boolean> => {
      if (!offlineOnly) {
        // Ensure user is logged in
        if (!currentUser || !isOnline) {
          setStartGameOpen(true);
          return false;
        }

        // Ensure user has a valid play key
        if (!userData?.playKey && !serverError) {
          setActivateOnlineOpen(true);
          return false;
        }

        if (userData?.playKey) {
          // Ensure the play key is saved to disk
          try {
            await backendService.assertPlayKey(userData.playKey);
          } catch (err) {
            showError(err);
            return false;
          }
        }
      }

      if (!isoPath) {
        showError(Messages.noMeleeIsoFile());
        return false;
      }

      // Only the two NTSC-U Brawl images are accepted (every player must run the same data).
      if (useIsoVerification.getState().validity === IsoValidity.INVALID) {
        showError(Messages.isoWillNotWork(PRODUCT_NAME));
        return false;
      }

      return await launchNetplay();
    },
    [
      currentUser,
      isOnline,
      launchNetplay,
      isoPath,
      userData,
      serverError,
      showError,
      backendService,
      setStartGameOpen,
      setActivateOnlineOpen,
    ],
  );
};
