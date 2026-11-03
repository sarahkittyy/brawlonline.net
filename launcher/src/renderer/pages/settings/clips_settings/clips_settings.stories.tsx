import "./clips_settings.stories.setup";

import { ClipHotkeyDialog } from "./clip_hotkey_dialog";
import { ClipsSettings } from "./clips_settings";

export default {
  title: "pages/Settings/ClipsSettings",
  argTypes: {},
};

const click = (name: string) =>
  document
    .querySelector<HTMLButtonElement>(`button[aria-label="${name}"]`)
    ?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/** Manual Clip Hotkey with L+R+A set; Configure opens the controller. */
export const Settings = () => <ClipsSettings />;

const Box = ({ current }: { current: number }) => (
  <ClipHotkeyDialog open={true} current={current} onClose={() => undefined} onApply={() => undefined} />
);

/** Configure with L+R+Z (the default): L, R and Z glow; Apply waits for a change. */
export const ConfigureDefault = () => <Box current={0x0070} />;

/** Z clicked off and A, D-pad Down clicked on: L+R+A+D-Pad Down, Apply on. */
export const ConfigureChanged = {
  render: () => <Box current={0x0070} />,
  play: async () => {
    await wait(300);
    click("Z");
    click("A");
    click("D-Pad Down");
  },
};

/** Every button clicked off: Apply waits for one. */
export const ConfigureEmpty = {
  render: () => <Box current={0x0070} />,
  play: async () => {
    await wait(300);
    click("L");
    click("R");
    click("Z");
  },
};
