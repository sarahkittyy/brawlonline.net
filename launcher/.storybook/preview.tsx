import "../src/renderer/styles/styles.scss"

import { fallbackTheme } from "../src/renderer/styles/theme";
import { setIconBase } from "../src/renderer/lib/icons";
import { ThemeProvider as MuiThemeProvider } from "@mui/material/styles";
import React from 'react';
import type { Preview } from "@storybook/react-webpack5";

// Badge icons from Storybook itself (main.ts staticDirs), not the accounts server.
setIconBase("");

const preview: Preview = {
  decorators: [
    (Story) => (
      <MuiThemeProvider theme={fallbackTheme}>
        <Story />
      </MuiThemeProvider>
    ),
  ],
  parameters: {
    actions: { argTypesRegex: "^on[A-Z].*" },
    controls: {
      matchers: {
        color: /(background|color)$/i,
        date: /Date$/,
      },
    },
  },
};

export default preview;
