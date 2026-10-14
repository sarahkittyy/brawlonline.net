import type { LiveGameEntry } from "@spectate/types";
import React from "react";

import styles from "./spectate.module.css";
import type { SpectateListState, SpectateViewProps } from "./spectate_view";
import { SpectateView } from "./spectate_view";

export default {
  title: "pages/Home/Spectate",
  argTypes: {},
  // The page fills the frame, so the viewport can be set to any window size.
  parameters: { layout: "fullscreen" },
};

const NOW = Date.parse("2026-10-14T02:10:00Z");
const ME = "me";

const player = (uid: string, displayName: string, characterId: number, wins = 0, teamId: number | null = null) => ({
  uid,
  displayName,
  connectCode: `${displayName.slice(0, 4).toUpperCase()}#1`,
  characterId,
  costume: 0,
  teamId,
  wins,
});

/** Every kind of row: Ranked game 1, a Direct set at 1-1, a room between games, a 2v2, the player's own game, a long name. */
const mockGames: LiveGameEntry[] = [
  {
    id: "aaaaaaaaaaaa",
    mode: "ranked",
    room: null,
    isTeams: false,
    startedAt: "2026-10-14T02:08:30Z",
    spectators: 214,
    game: { number: 1, place: 1, startedAt: "2026-10-14T02:08:30Z", stageId: 0x02, state: "playing" },
    players: [player("a1", "Drsmsh", 0x12), player("a2", "Sarah", 0x09)],
  },
  {
    id: "bbbbbbbbbbbb",
    mode: "direct",
    room: null,
    isTeams: false,
    startedAt: "2026-10-14T02:01:00Z",
    spectators: 3,
    game: { number: 3, place: 3, startedAt: "2026-10-14T02:07:15Z", stageId: 0x1f, state: "playing" },
    players: [player("b1", "Toon Link Main", 0x1d, 1), player("b2", "ギガクッパ", 0x0d, 1)],
  },
  {
    id: "cccccccccccc",
    mode: "room",
    room: "KFQB",
    isTeams: false,
    startedAt: "2026-10-14T01:58:00Z",
    spectators: 0,
    game: { number: 2, place: 2, startedAt: "2026-10-14T02:05:00Z", stageId: 0x03, state: "between" },
    players: [player("c1", "Lucas", 0x10, 1), player("c2", "Ness", 0x0f), player("c3", "Wario Fan", 0x15)],
  },
  {
    id: "dddddddddddd",
    mode: "unranked",
    room: null,
    isTeams: true,
    startedAt: "2026-10-14T01:55:00Z",
    spectators: 1,
    game: { number: 1, place: 1, startedAt: "2026-10-14T01:55:00Z", stageId: 0x05, state: "playing" },
    players: [
      player("d1", "ann", 0x02, 0, 0),
      player("d2", "bob", 0x09, 0, 0),
      player("d3", "cy", 0x04, 0, 1),
      player("d4", "dee", 0x06, 0, 1),
    ],
  },
  {
    id: "eeeeeeeeeeee",
    mode: "direct",
    room: null,
    isTeams: false,
    startedAt: "2026-10-14T01:50:00Z",
    spectators: 6,
    game: { number: 2, place: 2, startedAt: "2026-10-14T02:06:40Z", stageId: 0x02, state: "playing" },
    players: [player(ME, "me", 0x01, 1), player("e2", "A Very Long Display Name", 0x0a)],
  },
];

const Frame = ({ children }: { children: React.ReactNode }) => (
  <div className={styles.page} style={{ height: "100vh", width: "100vw" }}>
    {children}
  </div>
);

const props = (state: SpectateListState, extra: Partial<SpectateViewProps> = {}): SpectateViewProps => ({
  state,
  uid: ME,
  autoPlay: false,
  onAutoPlayChange: () => undefined,
  onWatch: () => undefined,
  onRetry: () => undefined,
  onLogIn: () => undefined,
  now: NOW,
  ...extra,
});

export const Ready = () => (
  <Frame>
    <SpectateView {...props({ kind: "ready", games: mockGames })} />
  </Frame>
);

export const Watching = () => (
  <Frame>
    <SpectateView
      {...props(
        { kind: "ready", games: mockGames },
        { autoPlay: true, watching: { status: "watching", stream: "bbbbbbbbbbbb", waitingForGame: false } },
      )}
    />
  </Frame>
);

export const Starting = () => (
  <Frame>
    <SpectateView
      {...props(
        { kind: "ready", games: mockGames },
        {
          startingId: "aaaaaaaaaaaa",
          watching: { status: "connecting", stream: "aaaaaaaaaaaa", waitingForGame: false },
        },
      )}
    />
  </Frame>
);

export const NewGame = () => (
  <Frame>
    <SpectateView {...props({ kind: "ready", games: mockGames }, { freshIds: new Set(["aaaaaaaaaaaa"]) })} />
  </Frame>
);

export const Empty = () => (
  <Frame>
    <SpectateView {...props({ kind: "ready", games: [] })} />
  </Frame>
);

export const Loading = () => (
  <Frame>
    <SpectateView {...props({ kind: "loading" })} />
  </Frame>
);

export const LoggedOut = () => (
  <Frame>
    <SpectateView {...props({ kind: "logged-out" })} />
  </Frame>
);

export const Unreachable = () => (
  <Frame>
    <SpectateView {...props({ kind: "unreachable" })} />
  </Frame>
);
