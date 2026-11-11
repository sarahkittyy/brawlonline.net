import { css } from "@emotion/react";
import PaletteOutlinedIcon from "@mui/icons-material/PaletteOutlined";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";

import { ThemeColorPickerMessages as Messages } from "./theme_color_picker.messages";
import { openThemeColorPickerHere } from "./use_theme_color_picker";

/** The header's palette button (right of the bug report button): the theme colour wheel over the current page. */
export const ThemeColorButton = () => (
  <Tooltip title={Messages.themeColor()}>
    <IconButton
      onClick={openThemeColorPickerHere}
      aria-label={Messages.themeColor()}
      css={css`
        opacity: 0.5;
      `}
      size="large"
    >
      <PaletteOutlinedIcon />
    </IconButton>
  </Tooltip>
);
