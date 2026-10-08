export const HOME_TABS = ["overview"] as const;
export type HomeTab = (typeof HOME_TABS)[number];

export const HomeRoutes = {
  overview: () => "/main/home/overview" as const,
};
