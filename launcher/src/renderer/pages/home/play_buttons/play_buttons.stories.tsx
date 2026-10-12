import { Tabs } from "../tabs/tabs";
import { PlayButtonsView } from "./play_buttons_view";

export default {
  title: "pages/Home/PlayButtons",
  argTypes: {},
};

const noop = () => undefined;

const QUEUES = { unranked: 6, ranked: 3 };
const PLAYING = { unranked: 14, ranked: 8 };

export const WithCounts = () => <PlayButtonsView queues={QUEUES} playing={PLAYING} onPlay={noop} />;

/** Three-digit counts: the widest the counts get in practice. */
export const BigCounts = () => (
  <PlayButtonsView queues={{ unranked: 104, ranked: 37 }} playing={{ unranked: 312, ranked: 186 }} onPlay={noop} />
);

/** Logged out, or the room list not read yet: the counts' space is kept, empty. */
export const CountsUnknown = () => <PlayButtonsView onPlay={noop} />;

/** As Home shows them: right after the Overview tab (home_page.tsx's TabRow). */
export const BesideTheTabs = () => (
  <div style={{ display: "flex", alignItems: "flex-start", gap: 8 }}>
    <Tabs value="overview" tabs={[{ id: "overview", label: "Overview" }]} />
    <PlayButtonsView queues={QUEUES} playing={PLAYING} onPlay={noop} />
  </div>
);

/** Play ranked was clicked and the game is being started (or asked). */
export const StartingRanked = () => (
  <PlayButtonsView
    queues={{ unranked: 0, ranked: 12 }}
    playing={{ unranked: 2, ranked: 20 }}
    starting="ranked"
    onPlay={noop}
  />
);
