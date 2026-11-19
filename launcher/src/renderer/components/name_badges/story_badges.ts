import type { NameBadge } from "@common/badges";

/**
 * Icons for the stories (fixtures/, served by Storybook at /v1/icons as accounts serves them; .storybook/main.ts):
 * a copy of the developers' `</>` and three made from MUI icons.
 */
export const STORY_ICONS = {
  dev: "8127fbba5bab2a89c3a1",
  trophy: "1e1bc14c536fa855ec9a",
  star: "98225a4d6a17a0eb69a9",
  heart: "3c0d508ebdd954867ab3",
} as const;

export const DEV_BADGE: NameBadge = { icon: STORY_ICONS.dev, label: "Dev" };
export const TROPHY_BADGE: NameBadge = { icon: STORY_ICONS.trophy };
export const STAR_BADGE: NameBadge = { icon: STORY_ICONS.star };
