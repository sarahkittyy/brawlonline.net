import { Tabs } from "../tabs/tabs";
import { PlayButtonsView } from "./play_buttons_view";

export default {
  title: "pages/Home/PlayButtons",
  argTypes: {},
};

const noop = () => undefined;

export const Default = () => <PlayButtonsView onPlay={noop} />;

/** As Home shows them: right after the Overview tab (home_page.tsx's TabRow). */
export const BesideTheTabs = () => (
  <div style={{ display: "flex", alignItems: "flex-start", gap: 8 }}>
    <Tabs value="overview" tabs={[{ id: "overview", label: "Overview" }]} />
    <PlayButtonsView onPlay={noop} />
  </div>
);

/** Play ranked was clicked and the game is being started (or asked). */
export const StartingRanked = () => <PlayButtonsView starting="ranked" onPlay={noop} />;
