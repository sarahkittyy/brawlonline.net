// Project Punch's state from main (dolphin/project_punch): the lab button on Home, its dialog and
// Settings > Training read it. A download goes on in main while the dialog is closed.
import type { ProjectPunchState } from "@dolphin/project_punch/types";
import log from "electron-log";
import { create } from "zustand";

type Store = {
  state: ProjectPunchState | null;
  dialogOpen: boolean;
};

export const useProjectPunch = create<Store>(() => ({ state: null, dialogOpen: false }));

export const openProjectPunchDialog = () => {
  useProjectPunch.setState({ dialogOpen: true });
  void refreshProjectPunch(true);
};
export const closeProjectPunchDialog = () => useProjectPunch.setState({ dialogOpen: false });

let subscribed = false;

/** Listens to main's state (once) and reads it. */
export function watchProjectPunch() {
  if (subscribed || !window.electron?.projectPunch) {
    return;
  }
  subscribed = true;
  window.electron.projectPunch.onState((state) => useProjectPunch.setState({ state }));
  void refreshProjectPunch(false);
}

/** Reads the state again; `refresh` asks GitHub for the latest release too. */
export async function refreshProjectPunch(refresh = true): Promise<void> {
  try {
    useProjectPunch.setState({ state: await window.electron.projectPunch.getState(refresh) });
  } catch (err) {
    log.warn(err);
  }
}

/** Download or update. A failure shows in the state's `error`; Cancel is no error. */
export async function installProjectPunch(): Promise<void> {
  try {
    const state = await window.electron.projectPunch.install();
    useProjectPunch.setState({ state });
  } catch (err) {
    if (!(err as { cancelled?: boolean }).cancelled) {
      log.warn(err);
    }
    await refreshProjectPunch(false);
  }
}

export const cancelProjectPunch = () => void window.electron.projectPunch.cancel().catch(log.warn);

export async function removeProjectPunch(): Promise<void> {
  await window.electron.projectPunch.remove();
  await refreshProjectPunch(false);
}
