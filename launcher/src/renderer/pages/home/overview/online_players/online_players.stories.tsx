import type { SocialPlayer, SocialState } from "@social/types";
import type { CSSProperties } from "react";
import { useEffect } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { Services } from "@/services/types";
import { useGameAssets } from "@/styles/game_theme";

import { OnlinePlayers } from "./online_players";

export default {
  title: "pages/Home/OnlinePlayers",
  argTypes: {},
};

const names = ["Bob", "Kim", "Lee", "Alexander", "Sora", "Bobby", "Marth Main", "kimchi", "Zelda", "Robin"];
// Some with a main shown, some without (a dot before the name), some with a flag.
const mains = ["fox", undefined, "sheik", "ike", undefined, "random", "marth", undefined, undefined, "wolf"];
const countries = ["us", "se", undefined, "jp", undefined, "us", "se", "br", "fr", undefined];
const players: SocialPlayer[] = names.map((name, i) => ({
  uid: `p${i}`,
  displayName: name,
  connectCode: `${name.replace(/\W/g, "").slice(0, 4).toUpperCase()}#${i + 10}`,
  main: mains[i],
  country: countries[i],
}));
const STOCKS = Object.fromEntries(
  ["fox", "sheik", "ike", "marth", "wolf"].map((c) => [c, { file: `stocks/${c}.png` }]),
);

function setUp() {
  useGameAssets.getState().setState({ manifest: { stocks: STOCKS } } as any);
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {} } } as any);
  useSocial.setState({ state, tabs: [] });
}

const state: SocialState = {
  status: "online",
  online: players,
  room: null,
  ignored: [],
  invites: [],
  conversations: {},
};

const toasts = { showError: () => {}, showInfo: () => {}, showWarning: () => {}, showSuccess: () => {} };
const { ServiceProvider } = createServiceProvider({
  services: { notificationService: toasts } as unknown as Services,
});

/** Ten players online: the magnifier next to the title opens the search box over the count. */
export const Default = () => {
  useEffect(setUp, []);
  return (
    <div style={{ width: 360, height: 420, display: "flex", flexDirection: "column", padding: 20 }}>
      <ServiceProvider>
        <OnlinePlayers />
      </ServiceProvider>
    </div>
  );
};

/**
 * The same list in the plain look and in P+'s colours (its menu gradient's palette, as
 * styles/game_theme.tsx makes it): the no-main dot has the accent's hue at a fixed lightness, so it is grey in one
 * and a muted green in the other.
 */
export const Themes = () => {
  useEffect(setUp, []);
  const pplus = {
    "--accent-primary": "#209a7a",
    "--accent-primary-bright": "#209a7a",
    "--accent-primary-dark": "#2773a1",
    "--surface-1": "#06211d",
    "--surface-2": "#092f2a",
    "--surface-3": "#2773a1",
    background: "#092f2a",
  } as CSSProperties;
  return (
    <ServiceProvider>
      <div style={{ display: "flex", gap: 20, padding: 20 }}>
        {[{}, pplus].map((vars, i) => (
          <div
            key={i}
            style={{ ...vars, width: 300, height: 460, display: "flex", flexDirection: "column", padding: 10 }}
          >
            <OnlinePlayers />
          </div>
        ))}
      </div>
    </ServiceProvider>
  );
};
