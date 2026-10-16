import { css } from "@emotion/react";
import { ThemeProvider } from "@mui/material/styles";
import React from "react";

import { createAppTheme, fallbackPalette } from "@/styles/theme";

import { Checkbox } from "./checkbox";
import { Toggle } from "./toggle";

export default {
  title: "components/Toggle",
  argTypes: {},
};

/** The palette prepareTheme samples from P+ 3.x's menu gradient (teal accent on a dark teal page). */
const pplusTheme = createAppTheme({
  ...fallbackPalette,
  primary: "#209a7a",
  secondary: "#2773a1",
  backgroundDefault: "#092f2a",
  backgroundPaper: "#06211d",
});

const Toggles = ({ background }: { background: string }) => {
  const [on, setOn] = React.useState(true);
  const [off, setOff] = React.useState(false);
  const [checked, setChecked] = React.useState(true);
  return (
    <div
      css={css`
        background: ${background};
        padding: 20px;
        width: 500px;
        display: flex;
        flex-direction: column;
        gap: 16px;
      `}
    >
      <Toggle value={on} onChange={setOn} label="Enable Auto Updates" description="On" />
      <Toggle value={off} onChange={setOff} label="Enable Netplay Replays" description="Off" />
      <Toggle
        value={true}
        onChange={() => undefined}
        label="Disabled, on"
        description="Dolphin is open"
        disabled={true}
      />
      <Toggle
        value={false}
        onChange={() => undefined}
        label="Disabled, off"
        description="Dolphin is open"
        disabled={true}
      />
      <Checkbox checked={checked} onChange={() => setChecked(!checked)} label="Organize into monthly subfolders" />
      <Checkbox checked={false} disabled={true} label="Organize into monthly subfolders (disabled)" />
    </div>
  );
};

export const ProjectPlusTheme = () => (
  <ThemeProvider theme={pplusTheme}>
    <Toggles background="#092f2a" />
  </ThemeProvider>
);

export const FallbackTheme = () => <Toggles background={fallbackPalette.backgroundDefault} />;
