// Settings > Add-ons (was Training): Project Punch and EmissaryRecomp, each a card with the same
// download / update / remove (docs/design/brawl-recompiled.md §4.1).
import { EMISSARY_REPO_URL } from "@dolphin/emissary/types";
import React, { useEffect, useState } from "react";
import { useNavigate } from "react-router-dom";

import { ConfirmationModal } from "@/components/confirmation_modal/confirmation_modal";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import {
  cancelEmissary,
  installEmissary,
  refreshEmissary,
  removeEmissary,
  useEmissary,
  watchEmissary,
} from "@/lib/emissary/use_emissary";
import { useEmissaryHint } from "@/lib/emissary/use_emissary_hint";
import { useEmissaryAutoUpdate, usePlayProjectPlusWith } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";

import { GameSettingsMessages } from "../game_settings/game_settings.messages";
import { TrainingSettings } from "../training_settings/training_settings";
import { AddonsSettingsMessages as Messages } from "./addons_settings.messages";
import { EmissarySettingView } from "./emissary_setting_view";
import { GAME_SETTINGS_PATH } from "./paths";

export const AddonsSettings = React.memo(function AddonsSettings() {
  const state = useEmissary((s) => s.state);
  const [autoUpdate, setAutoUpdate] = useEmissaryAutoUpdate();
  const [playWith, setPlayWith] = usePlayProjectPlusWith();
  const gameOpen = useDolphinStore((store) => store.netplayOpened);
  // The card glows on the first visit since EmissaryRecomp came (use_emissary_hint.ts), and the way here stops glowing.
  const [glow] = useState(() => !useEmissaryHint.getState().seen);
  const { showError } = useToasts();
  const navigate = useNavigate();
  const [busy, setBusy] = useState(false);
  const [confirmRemove, setConfirmRemove] = useState(false);

  useEffect(() => {
    watchEmissary();
    void refreshEmissary(true);
    useEmissaryHint.getState().markSeen();
  }, []);

  const run = async (fn: () => Promise<unknown>) => {
    setBusy(true);
    try {
      await fn();
    } catch (err) {
      showError(err);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div>
      <TrainingSettings />
      <EmissarySettingView
        state={state}
        autoUpdate={autoUpdate}
        busy={busy}
        onGet={() => void installEmissary()}
        onCancel={cancelEmissary}
        onRemove={() => setConfirmRemove(true)}
        onOpenFolder={() => state && void window.electron.shell.openPath(state.folder)}
        onOpenRepo={() => void window.electron.shell.openExternal(EMISSARY_REPO_URL)}
        onChooseDisc={() => navigate(GAME_SETTINGS_PATH)}
        onAutoUpdate={(value) => void setAutoUpdate(value)}
        playWithEmissary={playWith === "emissary"}
        playWithDisabledReason={gameOpen ? GameSettingsMessages.closeDolphinToChange() : undefined}
        onPlayWithEmissary={(on) => void setPlayWith(on ? "emissary" : "dolphin")}
        glow={glow && state?.supported !== false}
      />
      <ConfirmationModal
        open={confirmRemove}
        onClose={() => setConfirmRemove(false)}
        onSubmit={() => void run(removeEmissary)}
        title={Messages.remove()}
      >
        {Messages.removeConfirm()}
      </ConfirmationModal>
    </div>
  );
});
