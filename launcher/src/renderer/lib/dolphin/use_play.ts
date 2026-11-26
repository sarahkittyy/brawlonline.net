import { PRODUCT_NAME } from "@common/product";
import { IsoValidity } from "@common/types";
import { useCallback, useRef } from "react";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { HeaderMessages as Messages } from "@/app/header/header.messages";
import { emissaryNeedsSetUp, useEmissary } from "@/lib/emissary/use_emissary";
import type { EmissaryResume } from "@/lib/emissary/use_emissary_dialog";
import { useEmissaryDialog } from "@/lib/emissary/use_emissary_dialog";
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

type PlayOptions = { offlineOnly?: boolean; bootGame?: boolean; resume?: EmissaryResume };

/**
 * The Play button's checks and launch (Slippi's `onPlay`): logged in and online, a play key on
 * disk, a valid disc image, then the netplay Dolphin. Resolves to whether the game was started;
 * when a check fails it shows its dialog or error, as the Play button does. `offlineOnly` skips the
 * account checks (Play offline); `bootGame` boots P+ even when the Play button is set to open
 * Dolphin only (a hand-off: an invite, a room, Create room). `resume` is what EmissaryRecomp's popup runs when it
 * opens here and its Play is pressed (a hand-off's own retry); by default, this Play again.
 */
export const usePlay = () => {
  const { dolphinService, backendService } = useServices();
  const currentUser = useAccount((store) => store.user);
  const userData = useAccount((store) => store.userData);
  const serverError = useAccount((store) => store.serverError);
  const isoPath = useSettings((store) => store.settings.isoPath) || undefined;
  const playWith = useSettings((store) => store.settings.playProjectPlusWith);
  const isOnline = useAppStore((state) => state.isOnline);
  const { showError } = useToasts();
  const { launchNetplay } = useDolphinActions(dolphinService);
  const { setStartGameOpen, setActivateOnlineOpen } = usePlayDialogs();

  // This Play as it is at the latest render: what EmissaryRecomp's popup runs by default, once it is set up.
  const playRef = useRef<(options?: PlayOptions) => Promise<boolean>>(async () => false);

  const play = useCallback(
    async ({ offlineOnly, bootGame, resume }: PlayOptions = {}): Promise<boolean> => {
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

      // Play with EmissaryRecomp before it is set up: its popup, with Download (Project Punch's, on Home), not Dolphin.
      if (playWith === "emissary" && emissaryNeedsSetUp(useEmissary.getState().state)) {
        useEmissaryDialog.getState().openDialog(resume ?? (() => playRef.current({ offlineOnly, bootGame })));
        return false;
      }

      return await launchNetplay(bootGame);
    },
    [
      playWith,
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
  playRef.current = play;
  return play;
};
