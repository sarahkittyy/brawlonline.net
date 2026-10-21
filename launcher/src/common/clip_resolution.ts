/** A rendered video's height (Settings > Clips): 16:9, 720 by default. */
export const CLIP_RESOLUTIONS = [480, 720, 1080, 1440] as const;
export type ClipResolution = (typeof CLIP_RESOLUTIONS)[number];
export const DEFAULT_CLIP_RESOLUTION: ClipResolution = 720;

/** A setting's value as one of them (another value: the default). */
export function clipResolution(value: unknown): ClipResolution {
  return (CLIP_RESOLUTIONS as readonly unknown[]).includes(value) ? (value as ClipResolution) : DEFAULT_CLIP_RESOLUTION;
}
