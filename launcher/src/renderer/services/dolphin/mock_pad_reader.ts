import type { PadReaderEvent } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";

/** Keys standing in for a controller's buttons (GcButton bits, @common/clip_hotkey). */
const KEYS: Record<string, number> = {
  KeyQ: 0x40, // L
  KeyE: 0x20, // R
  KeyZ: 0x10,
  KeyA: 0x100,
  KeyB: 0x200,
  KeyX: 0x400,
  KeyY: 0x800,
  Enter: 0x1000, // Start
  ArrowUp: 0x8,
  ArrowDown: 0x4,
  ArrowLeft: 0x1,
  ArrowRight: 0x2,
};

/**
 * The pad reader of the mock services and stories (dolphin/pad_reader.ts in the app): the keyboard
 * is the controller, an adapter in port 1. Returns its stop.
 */
export function startMockPadReader(readerId: string, emit: (event: PadReaderEvent) => void): () => void {
  let buttons = 0;
  const send = (event: Omit<PadReaderEvent, "type" | "readerId">) =>
    emit({ type: DolphinEventType.PAD_READER, readerId, ...event });
  const onKey = (event: KeyboardEvent) => {
    const bit = KEYS[event.code];
    if (bit === undefined || event.repeat) {
      return;
    }
    buttons = event.type === "keydown" ? buttons | bit : buttons & ~bit;
    send({ buttons });
  };
  window.addEventListener("keydown", onKey);
  window.addEventListener("keyup", onKey);
  const timer = setTimeout(() => {
    send({
      status: { ports: [{ port: 1, kind: "adapter", device: "GameCube adapter", plugged: true }], adapter: "detected" },
    });
    send({ buttons });
  }, 300);
  return () => {
    clearTimeout(timer);
    window.removeEventListener("keydown", onKey);
    window.removeEventListener("keyup", onKey);
    send({ ended: { error: null } });
  };
}
