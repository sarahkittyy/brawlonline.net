export const HOME_TABS = ["overview", "spectate"] as const;
export type HomeTab = (typeof HOME_TABS)[number];

export const HomeRoutes = {
  overview: () => "/main/home/overview" as const,
  spectate: () => "/main/home/spectate" as const,
};
