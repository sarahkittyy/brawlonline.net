import type { Theme } from "@mui/material/styles";
import { createTheme } from "@mui/material/styles";

import { bodyFont, titleFont } from "./with_font";

/**
 * Colours the MUI theme needs as real values (it computes contrast and hover
 * shades from them). The fallback is neutral grey; with game assets loaded the
 * values are sampled from the extracted Brawl menu textures (game_theme.ts).
 */
export type ThemePalette = {
  primary: string;
  secondary: string;
  backgroundDefault: string;
  backgroundPaper: string;
  textPrimary: string;
  textSecondary: string;
};

export const fallbackPalette: ThemePalette = {
  primary: "#d0d0d0",
  secondary: "#bdbdbd",
  backgroundDefault: "#262626",
  backgroundPaper: "#1f1f1f",
  textPrimary: "#e9e9e9",
  textSecondary: "#b4b4b4",
};

/** Nine-slice frames from the game, as CSS values ready for `border-image`. */
export type ThemeFrames = {
  button?: FrameCss;
  buttonSelected?: FrameCss;
  panel?: FrameCss;
};

export type FrameCss = {
  /** `url("game-asset://cache/...")` */
  source: string;
  /** `border-image-slice`, e.g. "12 20 12 20 fill" */
  slice: string;
  /** `border-width` / `border-image-width`, e.g. "12px 20px 12px 20px" */
  width: string;
};

const frameStyles = (frame: FrameCss) => ({
  borderStyle: "solid",
  borderColor: "transparent",
  borderWidth: frame.width,
  borderImageSource: frame.source,
  borderImageSlice: frame.slice,
  borderImageWidth: frame.width,
  borderImageRepeat: "stretch",
  backgroundColor: "transparent",
  backgroundClip: "padding-box",
});

export function createAppTheme(palette: ThemePalette = fallbackPalette, frames: ThemeFrames = {}): Theme {
  const theme = createTheme({
    palette: {
      mode: "dark",
      text: {
        primary: palette.textPrimary,
        secondary: palette.textSecondary,
      },
      primary: {
        main: palette.primary,
      },
      secondary: {
        main: palette.secondary,
      },
      divider: "rgba(255,255,255)",
      background: {
        paper: palette.backgroundPaper,
        default: palette.backgroundDefault,
      },
    },
    typography: {
      fontFamily: bodyFont,
      fontSize: 16,
      h1: { fontFamily: titleFont },
      h2: { fontFamily: titleFont },
      h3: { fontFamily: titleFont },
      h4: { fontFamily: titleFont },
      h5: { fontFamily: titleFont },
      h6: { fontFamily: titleFont },
      caption: {
        opacity: 0.6,
      },
    },
  });
  return addOverrides(theme, palette, frames);
}

const addOverrides = (theme: Theme, palette: ThemePalette, frames: ThemeFrames) => {
  const containedButton = frames.button
    ? {
        ...frameStyles(frames.button),
        borderRadius: 0,
        boxShadow: "none",
        color: palette.textPrimary,
        "&:hover": frames.buttonSelected
          ? { ...frameStyles(frames.buttonSelected), boxShadow: "none" }
          : { backgroundColor: "transparent", filter: "brightness(1.15)", boxShadow: "none" },
        "&.Mui-disabled": { opacity: 0.5, color: palette.textSecondary },
      }
    : {};
  const panelPaper = frames.panel ? { ...frameStyles(frames.panel), borderRadius: 0 } : {};

  return createTheme({
    ...theme,
    components: {
      MuiTooltip: {
        defaultProps: {
          arrow: true,
        },
        styleOverrides: {
          arrow: {
            color: palette.textPrimary,
          },
          tooltip: {
            backgroundColor: palette.textPrimary,
            color: palette.backgroundPaper,
            boxShadow: theme.shadows[1],
            fontSize: 13,
          },
        },
      },
      MuiTextField: {
        defaultProps: {
          variant: "filled",
          fullWidth: true,
          size: "small",
        },
      },
      MuiRadio: {
        defaultProps: {
          color: "primary",
        },
      },
      MuiPaper: {
        styleOverrides: {
          root: {
            borderStyle: "solid",
            borderWidth: "1px",
            borderColor: "transparent",
          },
          rounded: {
            borderRadius: "10px",
            overflow: "hidden",
          },
        },
      },
      MuiDialog: {
        styleOverrides: {
          paper: panelPaper,
        },
      },
      MuiTableCell: {
        styleOverrides: {
          root: {
            borderBottomColor: "#1E1F25",
          },
        },
      },
      MuiInputLabel: {
        styleOverrides: {
          root: {
            color: "#dddddd",
            "&.Mui-focused": {
              color: "#ffffff",
            },
          },
        },
      },
      MuiListItemIcon: {
        styleOverrides: {
          root: {
            minWidth: "initial",
          },
        },
      },
      MuiMenuItem: {
        styleOverrides: {
          root: {
            "&.Mui-selected": {
              backgroundColor: "rgba(255, 255, 255, 0.16)",
            },
            "&.Mui-selected:hover": {
              backgroundColor: "rgba(255, 255, 255, 0.16)",
            },
          },
        },
      },
      MuiListItem: {
        styleOverrides: {
          root: {
            "&.Mui-selected": {
              backgroundColor: "rgba(255, 255, 255, 0.16)",
            },
            "&.Mui-selected:hover": {
              backgroundColor: "rgba(255, 255, 255, 0.16)",
            },
          },
        },
      },
      MuiButtonBase: {
        styleOverrides: {
          root: {
            fontFamily: bodyFont,
          },
        },
      },
      MuiButton: {
        styleOverrides: {
          root: {
            borderRadius: "10px",
          },
          contained: {
            fontWeight: 700,
            textTransform: "initial",
            borderRadius: "10px",
            ...containedButton,
          },
          // With game frames, labels are light like Brawl's menu keys (MUI would pick contrast text).
          containedPrimary: frames.button ? { color: palette.textPrimary, "&:hover": { color: "#ffffff" } } : {},
          containedSecondary: frames.button ? { color: palette.textPrimary, "&:hover": { color: "#ffffff" } } : {},
          outlined: {
            textTransform: "initial",
            borderRadius: "10px",
          },
        },
      },
    },
  });
};

/** The fallback theme, used before the game assets load and when there are none. */
export const fallbackTheme = createAppTheme();
