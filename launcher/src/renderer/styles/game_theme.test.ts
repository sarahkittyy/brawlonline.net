import type { GameAssetManifest } from "@game_assets/types";
import { describe, expect, it } from "vitest";

import { buildThemeCss, FRAME_SCALE, frameCss, framesFromManifest } from "./game_theme";
import { createAppTheme, fallbackPalette } from "./theme";

const texture = (file: string, width: number, height: number, inset: number) => ({
  file,
  width,
  height,
  origin: "sd",
  archive: "menu2/mu_menumain.pac",
  entry: "MiscData[4]",
  format: "IA4",
  greyscale: true,
  slice: { top: inset, right: inset, bottom: inset, left: inset },
});

// Shaped like the real extraction (MenCmn00 48x48 with 16-texel insets, MenSelchrEntryW01b 40x32 with 7).
const manifest: GameAssetManifest = {
  schemaVersion: 1,
  extractedAt: "2026-10-08T00:00:00.000Z",
  sources: { disc: "/disc" },
  textures: {
    button: texture("textures/button.png", 48, 48, 16),
    buttonSelected: texture("textures/buttonSelected.png", 40, 32, 7),
    panel: texture("textures/panel.png", 32, 24, 7),
  },
  fonts: {},
  stocks: {},
  missing: [],
};

const prepared = {
  palette: null,
  images: { button: "data:button", buttonHover: "data:buttonHover", panel: "data:panel" },
  progressTrack: "rgba(5, 32, 30, 0.7)",
};

describe("game theme frames", () => {
  it("slices in texels and sizes the border by the frame's display scale", () => {
    expect(FRAME_SCALE.button).toBe(0.5);
    expect(frameCss(manifest, "button")).toEqual({
      source: 'url("game-asset://cache/textures/button.png")',
      slice: "16 16 16 16 fill",
      width: "8px 8px 8px 8px",
    });
    expect(frameCss(manifest, "panel")?.width).toBe("7px 7px 7px 7px");
  });

  it("cuts the hover frame from the button plate, so only the image changes on hover", () => {
    const frames = framesFromManifest(manifest, prepared);
    expect(frames.buttonHover).toEqual({ ...frames.button, source: 'url("data:buttonHover")' });
    expect(frames.button?.source).toBe('url("data:button")');
  });

  it("emits matching CSS variables for the Play button and its hover", () => {
    const css = buildThemeCss(manifest, prepared);
    expect(css).toContain("--theme-button-width: 8px 8px 8px 8px;");
    expect(css).toContain("--theme-buttonHover-width: 8px 8px 8px 8px;");
    expect(css).toContain("--theme-buttonHover-slice: 16 16 16 16 fill;");
    expect(css).toContain('--theme-buttonHover-source: url("data:buttonHover");');
    expect(css).toContain("--theme-progress-track: rgba(5, 32, 30, 0.7);");
    expect(css).toContain("--theme-play-hover-opacity: 1;");
    expect(css).not.toContain("buttonSelected");
  });

  it("keeps a framed contained button the same size when hovered", () => {
    const frames = framesFromManifest(manifest, prepared);
    const theme = createAppTheme(fallbackPalette, frames);
    const contained = theme.components?.MuiButton?.styleOverrides?.contained as Record<string, any>;
    expect(contained.borderWidth).toBe("8px 8px 8px 8px");
    expect(contained.borderImageWidth).toBe("8px 8px 8px 8px");
    expect(contained["&:hover"]).toEqual({
      borderImageSource: 'url("data:buttonHover")',
      backgroundColor: "transparent",
      boxShadow: "none",
    });
  });

  it("leaves contained buttons alone without game frames", () => {
    const theme = createAppTheme(fallbackPalette, {});
    const contained = theme.components?.MuiButton?.styleOverrides?.contained as Record<string, any>;
    expect(contained.borderImageSource).toBeUndefined();
    expect(contained.borderRadius).toBe("10px");
  });
});
