import { Tabs } from "../tabs/tabs";
import { PlayButtonsView } from "./play_buttons_view";

export default {
  title: "pages/Home/PlayButtons",
  argTypes: {},
};

const noop = () => undefined;

export const Default = () => <PlayButtonsView twosQueue={2} twosPlaying={8} onPlay={noop} />;

/** Three-digit counts: the widest they get in practice. */
export const BigCounts = () => <PlayButtonsView twosQueue={23} twosPlaying={148} onPlay={noop} />;

/** Logged out, or the room list not read yet: the counts' space is kept, empty. */
export const CountsUnknown = () => <PlayButtonsView onPlay={noop} />;

/** As Home shows them: right after the Overview tab (home_page.tsx's TabRow). */
export const BesideTheTabs = () => (
  <div style={{ display: "flex", alignItems: "flex-start", gap: 8 }}>
    <Tabs value="overview" tabs={[{ id: "overview", label: "Overview" }]} />
    <PlayButtonsView twosQueue={2} twosPlaying={8} onPlay={noop} />
  </div>
);

/** Play ranked was clicked and the game is being started (or asked). */
export const StartingRanked = () => <PlayButtonsView twosQueue={0} twosPlaying={4} starting="ranked" onPlay={noop} />;
