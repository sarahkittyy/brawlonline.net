import type { ModsService } from "@dolphin/mods/types";

export default function createModsClient(): ModsService {
  return window.electron.mods;
}
