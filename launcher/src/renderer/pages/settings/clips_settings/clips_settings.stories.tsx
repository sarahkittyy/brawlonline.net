import "./clips_settings.stories.setup";

import type { DolphinEventType, PadReaderEvent } from "@dolphin/types";

import { createServiceProvider } from "@/services";
import { startMockPadReader } from "@/services/dolphin/mock_pad_reader";
import type { Services } from "@/services/types";

import { ClipHotkeyDialog } from "./clip_hotkey_dialog";
import { ClipsSettings } from "./clips_settings";

export default {
  title: "pages/Settings/ClipsSettings",
  argTypes: {},
};

// The keyboard is the controller (services/dolphin/mock_pad_reader.ts).
const listeners = new Set<(event: PadReaderEvent) => void>();
let stopReader: (() => void) | null = null;
const dolphinService = {
  startPadReader: async (readerId: string) => {
    stopReader?.();
    stopReader = startMockPadReader(readerId, (event) => listeners.forEach((l) => l(event)));
  },
  stopPadReader: async () => {
    stopReader?.();
    stopReader = null;
  },
  onEvent: (_type: DolphinEventType, handle: (event: PadReaderEvent) => void) => {
    listeners.add(handle);
    return () => listeners.delete(handle);
  },
};
const { ServiceProvider } = createServiceProvider({ services: { dolphinService } as unknown as Services });

const key = (type: "keydown" | "keyup", code: string) => window.dispatchEvent(new KeyboardEvent(type, { code }));
const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/** Manual Clip Hotkey with L+R+A set. Configure opens the box; the keyboard is the controller (Q L, E R, Z, A, B, X, Y, Enter Start, arrows). */
export const Settings = () => (
  <ServiceProvider>
    <ClipsSettings />
  </ServiceProvider>
);

const Box = () => (
  <ServiceProvider>
    <ClipHotkeyDialog open={true} onClose={() => undefined} onApply={() => undefined} />
  </ServiceProvider>
);

/** Configure, nothing pressed yet: Retry and Apply wait. */
export const ConfigureWaiting = { render: () => <Box /> };

/** L, R and A held: the hotkey grows until they are let go. */
export const ConfigureEntering = {
  render: () => <Box />,
  play: async () => {
    await wait(600);
    key("keydown", "KeyQ");
    key("keydown", "KeyE");
    await wait(100);
    key("keydown", "KeyA");
  },
};

/** L, R and A pressed and let go: finished, Retry and Apply on. */
export const ConfigureDone = {
  render: () => <Box />,
  play: async () => {
    await wait(600);
    key("keydown", "KeyQ");
    key("keydown", "KeyE");
    key("keydown", "KeyA");
    await wait(100);
    key("keyup", "KeyQ");
    key("keyup", "KeyA");
    key("keyup", "KeyE");
  },
};
