// EmissaryRecomp's popup, mounted once in app.tsx: Play opens it when Play Project+ with says EmissaryRecomp and it
// can't play yet (use_play.ts, emissaryNeedsSetUp). Its Play button, once it is installed, goes on with what opened it
// (the room, invite, Create room or queue; else Play).
import { EMISSARY_REPO_URL } from "@dolphin/emissary/types";
import React, { useEffect, useState } from "react";

import { usePlay } from "@/lib/dolphin/use_play";

import { EmissaryDialogView } from "./emissary_dialog_view";
import { cancelEmissary, installEmissary, useEmissary, watchEmissary } from "./use_emissary";
import { useEmissaryDialog } from "./use_emissary_dialog";

export const EmissaryDialog = React.memo(function EmissaryDialog() {
  const open = useEmissaryDialog((s) => s.open);
  const closeDialog = useEmissaryDialog((s) => s.closeDialog);
  const state = useEmissary((s) => s.state);
  const play = usePlay();
  const [starting, setStarting] = useState(false);

  // Its state from the start: Play reads it, and the way to Add-ons glows by it (use_emissary_hint.ts).
  useEffect(() => watchEmissary(), []);

  const onPlay = async () => {
    setStarting(true);
    try {
      const resume = useEmissaryDialog.getState().resume;
      if (await (resume ? resume() : play())) {
        closeDialog();
      }
    } finally {
      setStarting(false);
    }
  };

  return (
    <EmissaryDialogView
      open={open}
      state={state}
      starting={starting}
      onClose={closeDialog}
      onGet={() => void installEmissary()}
      onCancel={cancelEmissary}
      onPlay={() => void onPlay()}
      onOpenRepo={() => void window.electron.shell.openExternal(EMISSARY_REPO_URL)}
    />
  );
});
