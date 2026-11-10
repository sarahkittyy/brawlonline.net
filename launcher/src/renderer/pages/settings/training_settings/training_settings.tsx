// Settings > Training: Project Punch (dolphin/project_punch), as in Home's lab popup.
import { PROJECT_PUNCH_REPO_URL } from "@dolphin/project_punch/types";
import React, { useEffect, useState } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useToasts } from "@/lib/hooks/use_toasts";
import {
  cancelProjectPunch,
  installProjectPunch,
  refreshProjectPunch,
  removeProjectPunch,
  useProjectPunch,
  watchProjectPunch,
} from "@/lib/project_punch/use_project_punch";
import { useServices } from "@/services";

import { ProjectPunchSettingView } from "./project_punch_setting_view";

export const TrainingSettings = React.memo(function TrainingSettings() {
  const state = useProjectPunch((s) => s.state);
  const { dolphinService } = useServices();
  const { launchProjectPunch } = useDolphinActions(dolphinService);
  const { showError } = useToasts();
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    watchProjectPunch();
    void refreshProjectPunch(true);
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
      <ProjectPunchSettingView
        state={state}
        busy={busy}
        onGet={() => void installProjectPunch()}
        onCancel={cancelProjectPunch}
        onPlay={() => void run(launchProjectPunch)}
        onRemove={() => void run(removeProjectPunch)}
        onShowCard={() => state && window.electron.shell.showItemInFolder(state.cardPath)}
        onOpenRepo={() => void window.electron.shell.openExternal(PROJECT_PUNCH_REPO_URL)}
      />
    </div>
  );
});
