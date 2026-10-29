import type { RulesetView } from "@accounts/types";
import { enqueueSnackbar } from "notistack";
import React from "react";

import { SocialMessages } from "@/components/social/social.messages";
import { ToastProvider } from "@/components/toast_provider";
import type { PublicRoom } from "@/services/backend/types";

import overviewStyles from "../overview.module.css";
import type { PublicRoomsState, PublicRoomsViewProps } from "./public_rooms_view";
import { PublicRoomsView } from "./public_rooms_view";
import { RulesetBoxView } from "./ruleset_box";

export default {
  title: "pages/Home/PublicRooms",
  argTypes: {},
};

/** Every kind of row: waiting 1v1, FFA in game with an open slot, full Teams in game, long names. */
const mockRooms: PublicRoom[] = [
  {
    code: "KFQB",
    host: "Sarah",
    players: 1,
    openSlots: 2,
    mode: "1v1",
    status: "waiting",
    names: ["Sarah"],
    joinable: true,
    ruleset: {
      code: "KFQBTR",
      name: "NorCal Weekly",
      version: 3,
      by: "sarah",
      basedOn: null,
      bestOf: 3,
      stageChoice: "loser-picks",
    },
  },
  {
    code: "TRZM",
    host: "Wario Fan",
    players: 2,
    openSlots: 3,
    mode: "ffa",
    status: "waiting",
    names: ["Wario Fan", "Ness"],
    joinable: true,
    name: "chill ffa, no camping",
  },
  {
    code: "PLNX",
    host: "Lucas",
    players: 3,
    openSlots: 4,
    mode: "ffa",
    status: "in-game",
    names: ["Lucas", "Toon Link Main", "ギガクッパ"],
    joinable: true,
  },
  {
    code: "QWRT",
    host: "VeryLongName123",
    players: 2,
    openSlots: 2,
    mode: "1v1",
    status: "waiting",
    names: ["VeryLongName123", "AnotherLongName"],
    joinable: false,
    ruleset: {
      code: "MNPQRS",
      name: "Wednesday Night Fights Championship",
      version: 12,
      by: "VeryLongName123",
      basedOn: null,
      bestOf: 5,
      stageChoice: "loser-picks",
    },
  },
  {
    code: "HBPL",
    host: "Mango",
    players: 4,
    openSlots: 4,
    mode: "teams",
    status: "in-game",
    names: ["Mango", "Zain", "Cody", "Hungrybox"],
    joinable: false,
    name: "Friday doubles (all welcome)",
    ruleset: {
      code: null,
      name: "Custom",
      version: 0,
      by: "",
      basedOn: "NorCal Weekly v3",
      bestOf: 0,
      stageChoice: "loser-picks",
    },
  },
  {
    code: "MXWW",
    host: "WWWWWWWWWWWWWWW",
    players: 3,
    openSlots: 4,
    mode: "teams",
    status: "waiting",
    names: ["WWWWWWWWWWWWWWW", "MMMMMMMMMMMMMMM", "Pikachu Enjoyer"],
    joinable: true,
  },
];

const noop = () => undefined;
const showRuleset = (room: PublicRoom) => console.log("show ruleset", room.ruleset?.code); // eslint-disable-line no-console

/** The view in Home > Overview's grid, at the launcher's default window size. */
const Frame = ({
  width = 1100,
  ...props
}: Partial<PublicRoomsViewProps> & { state: PublicRoomsState; width?: number }) => (
  <div style={{ width, height: 560, background: "var(--theme-page-background)" }}>
    <div className={overviewStyles.container}>
      <PublicRoomsView
        onJoin={noop}
        onJoinCode={noop}
        onRetry={noop}
        onLogIn={noop}
        onShowRuleset={showRuleset}
        {...props}
      />
      <div className={overviewStyles.rankedSidebar} />
    </div>
  </div>
);

export const SeveralRooms = () => <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} />;

/** The player's room (PLNX, which they host) first, marked, with the pencil. */
export const YourRoom = () => (
  <Frame
    state={{ kind: "ready", online: 128, rooms: mockRooms }}
    yourRoom={{ kind: "pplus", code: "PLNX", host: true, name: null }}
    onRenameRoom={() => Promise.resolve()}
  />
);

/** The host typing its name. */
export const NamingYourRoom = () => (
  <Frame
    state={{ kind: "ready", online: 128, rooms: mockRooms }}
    yourRoom={{ kind: "pplus", code: "PLNX", host: true, name: null }}
    onRenameRoom={() => Promise.resolve()}
    forceEditText="lucas mains only"
  />
);

/** A member's room, named by its host: no pencil. */
export const YourRoomAsMember = () => (
  <Frame
    state={{ kind: "ready", online: 128, rooms: mockRooms }}
    yourRoom={{ kind: "pplus", code: "TRZM", host: false, name: "chill ffa, no camping" }}
    onRenameRoom={() => Promise.resolve()}
  />
);

export const OneRoom = () => <Frame state={{ kind: "ready", online: 1, rooms: mockRooms.slice(0, 1) }} />;

export const Empty = () => <Frame state={{ kind: "ready", online: 12, rooms: [] }} onCreate={noop} />;

/** A player who has never created a room: "Create room" glows. */
export const FirstRoom = () => (
  <Frame state={{ kind: "ready", online: 12, rooms: [] }} onCreate={noop} highlightCreate={true} />
);

/** The ruleset code box beside Create room: empty (its placeholder), and with a ruleset. */
export const RulesetBoxEmpty = () => (
  <Frame
    state={{ kind: "ready", online: 12, rooms: mockRooms }}
    onCreate={noop}
    rulesetBox={<RulesetBoxView ruleset={null} busy={false} onSubmit={noop} onClear={noop} />}
  />
);

export const RulesetBoxSet = () => (
  <Frame
    state={{ kind: "ready", online: 12, rooms: mockRooms }}
    onCreate={noop}
    rulesetBox={
      <RulesetBoxView
        ruleset={{ code: "QBHFSM", ruleset: { name: "NorCal Weekly", version: 1, by: "bob" } } as RulesetView}
        busy={false}
        onSubmit={noop}
        onClear={noop}
      />
    }
  />
);

export const Loading = () => <Frame state={{ kind: "loading" }} />;

export const Unreachable = () => <Frame state={{ kind: "unreachable" }} />;

export const LoggedOut = () => <Frame state={{ kind: "logged-out" }} />;

/** A joinable room under the mouse, and one being pressed. */
export const HoveredRow = () => (
  <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} forceRowState={{ code: "KFQB", state: "hover" }} />
);

export const PressedRow = () => (
  <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} forceRowState={{ code: "PLNX", state: "pressed" }} />
);

/** The click is being handed to the game. */
export const Joining = () => <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} joiningCode="TRZM" />;

/** The game is in a match: the launcher refuses with its message (a toast, as Play's errors are). */
export const GameBusy = () => {
  React.useEffect(() => {
    enqueueSnackbar(SocialMessages.finishGameFirst(), { variant: "warning", persist: true });
  }, []);
  return (
    <ToastProvider>
      <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} />
    </ToastProvider>
  );
};

/** A window of 900 px (the launcher allows 980 px; a larger Interface Size is narrower still). */
export const NarrowWindow = () => <Frame width={900} state={{ kind: "ready", online: 128, rooms: mockRooms }} />;

/** The narrowest window with the whole header: a long ruleset name, its code and Create room. */
export const NarrowWindowRulesetSet = () => (
  <Frame
    width={900}
    state={{ kind: "ready", online: 7, rooms: mockRooms }}
    onCreate={noop}
    rulesetBox={
      <RulesetBoxView
        ruleset={
          {
            code: "HBXLTQ",
            ruleset: { name: "final destination all items", version: 1, by: "sarahkitty" },
          } as RulesetView
        }
        busy={false}
        onSubmit={noop}
        onClear={noop}
        onDetails={noop}
      />
    }
  />
);
