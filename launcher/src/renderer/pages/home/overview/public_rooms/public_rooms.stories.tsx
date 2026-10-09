import { enqueueSnackbar } from "notistack";
import React from "react";

import { ToastProvider } from "@/components/toast_provider";
import type { PublicRoom } from "@/services/backend/types";

import overviewStyles from "../overview.module.css";
import { PublicRoomsMessages as Messages } from "./public_rooms.messages";
import type { PublicRoomsState, PublicRoomsViewProps } from "./public_rooms_view";
import { PublicRoomsView } from "./public_rooms_view";

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

/** The view in Home > Overview's grid, at the launcher's default window size. */
const Frame = ({
  width = 1100,
  ...props
}: Partial<PublicRoomsViewProps> & { state: PublicRoomsState; width?: number }) => (
  <div style={{ width, height: 560, background: "var(--theme-page-background)" }}>
    <div className={overviewStyles.container}>
      <PublicRoomsView onJoin={noop} onRetry={noop} onLogIn={noop} {...props} />
      <div className={overviewStyles.rankedSidebar} />
    </div>
  </div>
);

export const SeveralRooms = () => <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} />;

export const OneRoom = () => <Frame state={{ kind: "ready", online: 1, rooms: mockRooms.slice(0, 1) }} />;

export const Empty = () => <Frame state={{ kind: "ready", online: 12, rooms: [] }} />;

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
    enqueueSnackbar(Messages.finishGameFirst(), { variant: "warning", persist: true });
  }, []);
  return (
    <ToastProvider>
      <Frame state={{ kind: "ready", online: 128, rooms: mockRooms }} />
    </ToastProvider>
  );
};

/** The narrowest window the launcher allows (900 px). */
export const NarrowWindow = () => <Frame width={900} state={{ kind: "ready", online: 128, rooms: mockRooms }} />;
