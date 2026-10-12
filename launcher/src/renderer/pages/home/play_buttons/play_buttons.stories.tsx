import { Tabs } from "../tabs/tabs";
import { PlayButtonsView } from "./play_buttons_view";

export default {
  title: "pages/Home/PlayButtons",
  argTypes: {},
};

const noop = () => undefined;

export const WithCounts = () => <PlayButtonsView queues={{ unranked: 6, ranked: 3 }} onPlay={noop} />;

/** Logged out, or the room list not read yet: the counts' space is kept, empty. */
export const CountsUnknown = () => <PlayButtonsView onPlay={noop} />;

/** As Home shows them: right after the Overview tab (home_page.tsx's TabRow). */
export const BesideTheTabs = () => (
  <div style={{ display: "flex", alignItems: "flex-start", gap: 8 }}>
    <Tabs value="overview" tabs={[{ id: "overview", label: "Overview" }]} />
    <PlayButtonsView queues={{ unranked: 6, ranked: 3 }} onPlay={noop} />
  </div>
);

/** Play ranked was clicked and the game is being started (or asked). */
export const StartingRanked = () => (
  <PlayButtonsView queues={{ unranked: 0, ranked: 12 }} starting="ranked" onPlay={noop} />
);
