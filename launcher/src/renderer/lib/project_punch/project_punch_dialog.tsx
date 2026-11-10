// Project Punch's popup, mounted once in app.tsx; Home's lab button opens it (use_project_punch.ts).
import { PROJECT_PUNCH_REPO_URL } from "@dolphin/project_punch/types";
import React, { useState } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useServices } from "@/services";

import { ProjectPunchDialogView } from "./project_punch_dialog_view";
import {
  cancelProjectPunch,
  closeProjectPunchDialog,
  installProjectPunch,
  refreshProjectPunch,
  useProjectPunch,
} from "./use_project_punch";

export const ProjectPunchDialog = React.memo(function ProjectPunchDialog() {
  const open = useProjectPunch((s) => s.dialogOpen);
  const state = useProjectPunch((s) => s.state);
  const { dolphinService } = useServices();
  const { launchProjectPunch } = useDolphinActions(dolphinService);
  const [starting, setStarting] = useState(false);

  const play = async () => {
    setStarting(true);
    try {
      if (await launchProjectPunch()) {
        closeProjectPunchDialog();
      }
    } finally {
      setStarting(false);
    }
  };

  return (
    <ProjectPunchDialogView
      open={open}
      state={state}
      starting={starting}
      onClose={closeProjectPunchDialog}
      onGet={() => void installProjectPunch()}
      onCancel={cancelProjectPunch}
      onPlay={() => void play()}
      onRetry={() => void refreshProjectPunch(true)}
      onOpenRepo={() => void window.electron.shell.openExternal(PROJECT_PUNCH_REPO_URL)}
    />
  );
});
