import { ThemeProvider } from "@emotion/react";
import type { GameAssetManifest, GameAssetsState } from "@game_assets/types";
import { GAME_ASSET_BASE_URL } from "@game_assets/types";
import type { Theme } from "@mui/material/styles";
import { ThemeProvider as MuiThemeProvider } from "@mui/material/styles";
import log from "electron-log";
import React from "react";
import { create } from "zustand";

import type { FrameCss, ThemeFrames, ThemePalette } from "./theme";
import { createAppTheme, fallbackPalette, fallbackTheme } from "./theme";
import { SYSTEM_FONT_STACK } from "./with_font";

/**
 * Builds the launcher's look from the game assets the user's own disc and SD
 * card provided (src/game_assets). Nothing here draws or invents graphics:
 *
 * - Fonts are Brawl's RFNT menu/title fonts converted to TrueType.
 * - Brawl's menu frames are greyscale masks the game tints at runtime. We tint
 *   them the same way (multiply) with colours sampled from P+'s own menu
 *   background gradient texture, so every colour also comes from the game.
 * - The page background is that gradient with P+'s grid tile over it, as on
 *   P+'s character and stage select screens.
 *
 * Without a manifest the plain fallback from _variables.scss / theme.ts stays.
 */

export const assetUrl = (file: string) => `${GAME_ASSET_BASE_URL}${file.split("/").map(encodeURIComponent).join("/")}`;

const STYLE_ELEMENT_ID = "game-theme";
const MENU_FONT_FAMILY = "GameMenuFont";
const TITLE_FONT_FAMILY = "GameTitleFont";
const FRAME_KEYS = ["button", "buttonSelected", "panel"] as const;

type RGB = [number, number, number];

export const useGameAssets = create<{ state: GameAssetsState | null; setState: (s: GameAssetsState) => void }>(
  (set) => ({
    state: null,
    setState: (state) => set({ state }),
  }),
);

/** Stock icon URL for a character key from the manifest (e.g. "mario"), if extracted. */
export function useStockIconUrl(character: string | undefined): string | undefined {
  const manifest = useGameAssets((s) => s.state?.manifest);
  if (!character || !manifest) {
    return undefined;
  }
  const stock = manifest.stocks[character];
  return stock ? assetUrl(stock.file) : undefined;
}

/** Image URLs (tinted copies where needed) and colours derived from the manifest. */
export type PreparedTheme = {
  palette: ThemePalette | null;
  images: Partial<Record<string, string>>;
};

export function frameCss(manifest: GameAssetManifest, key: string, imageUrl?: string): FrameCss | undefined {
  const tex = manifest.textures[key];
  if (!tex) {
    return undefined;
  }
  const s = tex.slice ?? { top: 0, right: 0, bottom: 0, left: 0 };
  return {
    source: `url("${imageUrl ?? assetUrl(tex.file)}")`,
    slice: `${s.top} ${s.right} ${s.bottom} ${s.left} fill`,
    width: `${s.top}px ${s.right}px ${s.bottom}px ${s.left}px`,
  };
}

export function framesFromManifest(manifest: GameAssetManifest, prepared?: PreparedTheme): ThemeFrames {
  return {
    button: frameCss(manifest, "button", prepared?.images.button),
    buttonSelected: frameCss(manifest, "buttonSelected", prepared?.images.buttonSelected),
    panel: frameCss(manifest, "panel", prepared?.images.panel),
  };
}

/** CSS for fonts and asset variables; applied on :root. */
export function buildThemeCss(manifest: GameAssetManifest, prepared: PreparedTheme | null): string {
  const lines: string[] = [];
  const menuFont = manifest.fonts.menuFont;
  const titleFont = manifest.fonts.titleFont ?? menuFont;
  if (menuFont) {
    lines.push(`@font-face { font-family: "${MENU_FONT_FAMILY}"; src: url("${assetUrl(menuFont.file)}"); }`);
  }
  if (titleFont) {
    lines.push(`@font-face { font-family: "${TITLE_FONT_FAMILY}"; src: url("${assetUrl(titleFont.file)}"); }`);
  }

  const vars: string[] = [];
  if (menuFont) {
    vars.push(`--font-body: "${MENU_FONT_FAMILY}", ${SYSTEM_FONT_STACK};`);
  }
  if (titleFont) {
    vars.push(`--font-title: "${TITLE_FONT_FAMILY}", ${SYSTEM_FONT_STACK};`);
  }

  if (prepared) {
    const layers: string[] = [];
    if (prepared.images.backgroundTile) {
      layers.push(`url("${prepared.images.backgroundTile}") repeat top left / 32px 32px`);
    }
    const bg = manifest.textures.background;
    if (bg) {
      layers.push(`url("${assetUrl(bg.file)}") no-repeat center / 100% 100%`);
    }
    if (layers.length > 0) {
      vars.push(`--theme-background: ${layers.join(", ")};`);
    }
    for (const key of FRAME_KEYS) {
      const frame = frameCss(manifest, key, prepared.images[key]);
      if (frame) {
        vars.push(`--theme-${key}-source: ${frame.source};`);
        vars.push(`--theme-${key}-slice: ${frame.slice};`);
        vars.push(`--theme-${key}-width: ${frame.width};`);
      }
    }
    if (prepared.images.cursor) {
      vars.push(`--theme-cursor-image: url("${prepared.images.cursor}");`);
    }
    const p = prepared.palette;
    if (p) {
      vars.push(`--accent-primary: ${p.primary};`);
      vars.push(`--accent-primary-bright: ${p.primary};`);
      vars.push(`--accent-primary-dark: ${p.secondary};`);
      vars.push(`--accent-primary-darker: ${p.secondary};`);
      vars.push(`--accent-secondary: ${p.secondary};`);
      vars.push(`--surface-0: ${p.backgroundPaper};`);
      vars.push(`--surface-1: ${p.backgroundPaper};`);
      vars.push(`--surface-2: ${p.backgroundDefault};`);
      vars.push(`--surface-3: ${p.secondary};`);
      vars.push(`--surface-4: ${p.primary};`);
      vars.push(`--theme-page-background: ${p.backgroundDefault};`);
    }
  }
  lines.push(`:root { ${vars.join(" ")} }`);
  return lines.join("\n");
}

function applyCss(css: string | null) {
  let el = document.getElementById(STYLE_ELEMENT_ID) as HTMLStyleElement | null;
  if (!css) {
    el?.remove();
    return;
  }
  if (!el) {
    el = document.createElement("style");
    el.id = STYLE_ELEMENT_ID;
    document.head.appendChild(el);
  }
  el.textContent = css;
}

async function loadPixels(url: string): Promise<{ canvas: HTMLCanvasElement; data: ImageData } | null> {
  try {
    const img = new Image();
    img.crossOrigin = "anonymous";
    img.src = url;
    await img.decode();
    const canvas = document.createElement("canvas");
    canvas.width = img.naturalWidth;
    canvas.height = img.naturalHeight;
    const ctx = canvas.getContext("2d");
    if (!ctx) {
      return null;
    }
    ctx.drawImage(img, 0, 0);
    return { canvas, data: ctx.getImageData(0, 0, canvas.width, canvas.height) };
  } catch (err) {
    log.warn(`Could not load ${url}`, err);
    return null;
  }
}

/**
 * Multiplies a greyscale mask by a colour, as Brawl's TEV stage does for menu
 * frames. `alpha` scales the result's opacity.
 */
async function tint(url: string, [r, g, b]: RGB, alpha = 1): Promise<string | undefined> {
  const loaded = await loadPixels(url);
  if (!loaded) {
    return undefined;
  }
  const { canvas, data } = loaded;
  const px = data.data;
  for (let i = 0; i < px.length; i += 4) {
    px[i] = (px[i] * r) / 255;
    px[i + 1] = (px[i + 1] * g) / 255;
    px[i + 2] = (px[i + 2] * b) / 255;
    px[i + 3] = px[i + 3] * alpha;
  }
  canvas.getContext("2d")!.putImageData(data, 0, 0);
  return canvas.toDataURL("image/png");
}

const clamp = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
const hex = ([r, g, b]: RGB) => "#" + [r, g, b].map((v) => clamp(v).toString(16).padStart(2, "0")).join("");
const luminance = ([r, g, b]: RGB) => (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
/** Scales a colour so its luminance reaches `target` (keeps the hue; only for legibility). */
const toLuminance = (c: RGB, target: number): RGB => {
  const f = target / Math.max(luminance(c), 0.01);
  return [c[0] * f, c[1] * f, c[2] * f];
};

/** Colours at the top, middle and bottom of the vertical gradient texture. */
async function sampleGradient(url: string): Promise<{ top: RGB; middle: RGB; bottom: RGB } | null> {
  const loaded = await loadPixels(url);
  if (!loaded) {
    return null;
  }
  const { data } = loaded;
  const at = (y: number): RGB => {
    const i = (Math.min(data.height - 1, Math.max(0, y)) * data.width + Math.floor(data.width / 2)) * 4;
    return [data.data[i], data.data[i + 1], data.data[i + 2]];
  };
  return { top: at(0), middle: at(Math.floor(data.height / 2)), bottom: at(data.height - 1) };
}

/**
 * Palette from P+'s menu background gradient (teal, dark, blue in P+ 3.x): the
 * accent is its top colour, the secondary its bottom colour, the surfaces its
 * dark middle. A disc-only extraction has no gradient and keeps the neutral palette.
 */
export async function prepareTheme(manifest: GameAssetManifest): Promise<PreparedTheme> {
  const images: PreparedTheme["images"] = {};
  const background = manifest.textures.background;
  const gradient = background ? await sampleGradient(assetUrl(background.file)) : null;

  let palette: ThemePalette | null = null;
  let frameTint: RGB = [255, 255, 255];
  let selectedTint: RGB = [255, 255, 255];
  let panelTint: RGB = [255, 255, 255];
  if (gradient) {
    const accent = toLuminance(gradient.top, Math.max(luminance(gradient.top), 0.45));
    const secondary = toLuminance(gradient.bottom, Math.max(luminance(gradient.bottom), 0.4));
    palette = {
      ...fallbackPalette,
      primary: hex(accent),
      secondary: hex(secondary),
      backgroundDefault: hex(gradient.middle),
      backgroundPaper: hex(toLuminance(gradient.middle, luminance(gradient.middle) * 0.7)),
    };
    frameTint = gradient.top;
    selectedTint = toLuminance(gradient.top, Math.min(0.85, luminance(gradient.top) * 1.6));
    panelTint = gradient.bottom;
  }

  const tinted: Array<[string, RGB, number]> = [
    ["button", frameTint, 1],
    ["buttonSelected", selectedTint, 1],
    ["panel", panelTint, 1],
    ["backgroundTile", [255, 255, 255], 0.12],
    ["cursor", [255, 255, 255], 1],
  ];
  for (const [key, color, alpha] of tinted) {
    const tex = manifest.textures[key];
    if (tex) {
      images[key] = (await tint(assetUrl(tex.file), color, alpha)) ?? assetUrl(tex.file);
    }
  }
  return { palette, images };
}

/**
 * Provides the MUI/emotion theme and keeps it in sync with the extracted game
 * assets. Renders the fallback theme until (and unless) assets are available.
 */
export const GameThemeProvider = ({ children }: React.PropsWithChildren) => {
  const [theme, setTheme] = React.useState<Theme>(fallbackTheme);
  const assetsState = useGameAssets((s) => s.state);

  React.useEffect(() => {
    const api = window.electron?.gameAssets;
    if (!api) {
      return;
    }
    const setState = useGameAssets.getState().setState;
    void api.getState().then(setState).catch(log.error);
    return api.onStateChange(setState);
  }, []);

  const manifest = assetsState?.manifest ?? null;
  React.useEffect(() => {
    let cancelled = false;
    if (!manifest) {
      applyCss(null);
      setTheme(fallbackTheme);
      return;
    }
    // Fonts first, then frames and colours once the textures are tinted.
    applyCss(buildThemeCss(manifest, null));
    void prepareTheme(manifest)
      .then((prepared) => {
        if (cancelled) {
          return;
        }
        applyCss(buildThemeCss(manifest, prepared));
        setTheme(createAppTheme(prepared.palette ?? fallbackPalette, framesFromManifest(manifest, prepared)));
      })
      .catch(log.error);
    return () => {
      cancelled = true;
    };
  }, [manifest]);

  return (
    <MuiThemeProvider theme={theme}>
      <ThemeProvider theme={theme as any}>{children}</ThemeProvider>
    </MuiThemeProvider>
  );
};
