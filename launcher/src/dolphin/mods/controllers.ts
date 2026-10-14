import path from "path";

import { IniFile } from "../config/ini_file";
import type { DolphinControllers, ModsController, SetUpController } from "./types";

/** Dolphin's SI device numbers (SI_Device.h) for a GameCube port. */
const SIDEVICE_GC_CONTROLLER = 6;
const SIDEVICE_WIIU_ADAPTER = 12;
/** Our Dolphin's default for a port with no setting: the GameCube controller adapter. */
const SIDEVICE_DEFAULT = SIDEVICE_WIIU_ADAPTER;
/** Dolphin's Wii Remote sources (Wiimote.h); a Wii Remote with no setting is None. */
const WIIMOTE_EMULATED = 1;
const WIIMOTE_REAL = 2;

/** A device as Dolphin names it (`XInput/0/Gamepad`, `SDL/1/Xbox Series X Controller`) the way a
 * player calls it. */
export function deviceLabel(device: string): string {
  const parts = device.split("/");
  if (parts.length < 3) {
    return device || "No device";
  }
  const [source, id] = parts;
  const name = parts.slice(2).join("/");
  const nth = id !== "0" && /^\d+$/.test(id) ? ` ${Number(id) + 1}` : "";
  if (/keyboard/i.test(name)) {
    return "Keyboard";
  }
  if (source === "XInput") {
    return `Xbox controller${nth}`;
  }
  if (source === "Bluetooth" && name === "Wii Remote") {
    return `Wii Remote${nth}`;
  }
  return `${name}${nth}`;
}

/** The extension Dolphin's emulated Wii Remote has (`[WiimoteN] Extension`), for its label. */
function extensionLabel(extension: string): string | null {
  switch (extension.trim().toLowerCase()) {
    case "nunchuk":
      return "Nunchuk";
    case "classic":
      return "Classic Controller";
    default:
      return null;
  }
}

/**
 * The controllers set up in Dolphin's Controllers settings (the netplay User folder's
 * `Config/Dolphin.ini`, `GCPadNew.ini`, `WiimoteNew.ini`), folded by kind: the GameCube ports
 * that have a standard controller or the adapter, and the Wii Remotes that are emulated or real,
 * in order. Dolphin plays the first of the kind plugged in (Machine::PickLocal).
 */
export async function readDolphinControllers(userFolder: string): Promise<DolphinControllers> {
  const config = path.join(userFolder, "Config");
  const [dolphin, gcPads, wiimotes] = await Promise.all([
    IniFile.init(path.join(config, "Dolphin.ini")),
    IniFile.init(path.join(config, "GCPadNew.ini")),
    IniFile.init(path.join(config, "WiimoteNew.ini")),
  ]);
  const gc: SetUpController[] = [];
  const core = dolphin.getSection("Core");
  for (let i = 0; i < 4; i++) {
    const si = Number(core?.get(`SIDevice${i}`, String(SIDEVICE_DEFAULT)) ?? SIDEVICE_DEFAULT);
    if (si === SIDEVICE_WIIU_ADAPTER) {
      gc.push({ index: i + 1, device: "GameCube adapter", label: "GameCube adapter" });
    } else if (si === SIDEVICE_GC_CONTROLLER) {
      const device = gcPads.getSection(`GCPad${i + 1}`)?.get("Device", "") ?? "";
      gc.push({ index: i + 1, device, label: deviceLabel(device) });
    }
  }
  const wiimote: SetUpController[] = [];
  for (let i = 0; i < 4; i++) {
    const section = wiimotes.getSection(`Wiimote${i + 1}`);
    const source = Number(section?.get("Source", "0") ?? 0);
    if (source === WIIMOTE_REAL) {
      wiimote.push({ index: i + 1, device: "", label: "Real Wii Remote", real: true });
    } else if (source === WIIMOTE_EMULATED) {
      const device = section?.get("Device", "") ?? "";
      const extension = extensionLabel(section?.get("Extension", "") ?? "");
      const label = deviceLabel(device);
      wiimote.push({ index: i + 1, device, label: extension ? `${label} + ${extension}` : label });
    }
  }
  return { gc, wiimote };
}

/** The kind to play when the player never picked one: a Wii Remote if only that is set up. */
export function defaultControllerKind(controllers: DolphinControllers): ModsController {
  return controllers.wiimote.length > 0 && controllers.gc.length === 0 ? "wiimote" : "gc";
}
