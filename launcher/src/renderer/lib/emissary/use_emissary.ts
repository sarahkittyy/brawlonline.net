// EmissaryRecomp's state from main (dolphin/emissary): Settings > Add-ons and Settings > Game read
// it. A download goes on in main while the page is closed.
import type { EmissaryState } from "@dolphin/emissary/types";
import log from "electron-log";
import { create } from "zustand";

type Store = { state: EmissaryState | null };

export const useEmissary = create<Store>(() => ({ state: null }));

let subscribed = false;

/** Listens to main's state (once) and reads it. */
export function watchEmissary() {
  if (subscribed || !window.electron?.emissary) {
    return;
  }
  subscribed = true;
  window.electron.emissary.onState((state) => useEmissary.setState({ state }));
  void refreshEmissary(false);
}

/** Reads the state again; `refresh` asks GitHub for the latest release too. */
export async function refreshEmissary(refresh = true): Promise<void> {
  try {
    useEmissary.setState({ state: await window.electron.emissary.getState(refresh) });
  } catch (err) {
    log.warn(err);
  }
}

/** Download, update or prepare the game files. A failure shows in the state's `error`; Cancel is no error. */
export async function installEmissary(): Promise<void> {
  try {
    useEmissary.setState({ state: await window.electron.emissary.install() });
  } catch (err) {
    if (!(err as { cancelled?: boolean }).cancelled) {
      log.warn(err);
    }
    await refreshEmissary(false);
  }
}

export const cancelEmissary = () => void window.electron.emissary.cancel().catch(log.warn);

export async function removeEmissary(): Promise<void> {
  await window.electron.emissary.remove();
  await refreshEmissary(false);
}

/** The card's main button for a state: what it does, or null for none. */
export function emissaryAction(state: EmissaryState | null): "download" | "update" | "prepare" | null {
  switch (state?.status) {
    case "notInstalled":
      return "download";
    case "updateAvailable":
      return "update";
    case "needsFiles":
      return "prepare";
    default:
      return null;
  }
}

export const emissaryJobPercent = (job: EmissaryState["job"]) =>
  job && job.total > 0 ? Math.min(100, (job.current / job.total) * 100) : 0;

/**
 * Play with EmissaryRecomp chosen can't start it until it is set up: nothing installed, or its game files not there
 * (missing, or being extracted). Play then opens its popup (emissary_dialog.tsx) instead of falling back to Dolphin
 * (Sarah's board, 2026-11-24: "the same exact popup you do when you click the project punch icon and don't have it
 * installed"). An installed release downloading its update is not this: Play waits for that update in main.
 */
export function emissaryNeedsSetUp(state: EmissaryState | null): boolean {
  switch (state?.status) {
    case "notInstalled":
    case "needsFiles":
    case "preparingFiles":
      return true;
    case "downloading":
      return state.installed == null;
    default:
      return false;
  }
}
