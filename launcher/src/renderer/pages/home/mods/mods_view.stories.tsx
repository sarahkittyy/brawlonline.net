import type { ModBuild, ModRoomStatus, ModsState } from "@dolphin/mods/types";

import type { ModsViewProps } from "./mods_view";
import { ModsView } from "./mods_view";

export default {
  title: "pages/Home/BrawlMods",
  argTypes: {},
};

const noop = () => undefined;

const builds: ModBuild[] = [
  {
    id: "vanilla",
    name: "Brawl (vanilla)",
    discPath: "C:/Games/Super Smash Bros. Brawl (USA) (Rev 2).iso",
    launcherPath: null,
    sdPath: null,
    hash: "8e21d0c37aa00000000000000000000000000000000000000000000000000001",
    hashKey: "k",
  },
  {
    id: "minus",
    name: "Brawl Minus 5.1",
    discPath: "C:/Games/Super Smash Bros. Brawl (USA) (Rev 2).iso",
    launcherPath: "C:/Games/Minus/MinusLauncher/minus.dol",
    sdPath: "C:/Games/Minus/sd.raw",
    hash: "3f9a1c07aa000000000000000000000000000000000000000000000000beef",
    hashKey: "k",
  },
  {
    id: "rex",
    name: "REX",
    discPath: "C:/Games/Super Smash Bros. Brawl (USA) (Rev 2).iso",
    launcherPath: "C:/Games/REX/launcher.dol",
    sdPath: "C:/Games/REX/sd.raw",
    hash: null,
    hashKey: null,
  },
];

const room = (you: number, extra: Partial<ModRoomStatus> = {}): ModRoomStatus => ({
  version: 1,
  kind: you === 1 ? "host" : "join",
  state: "in",
  error: "",
  room: {
    code: "KFQB",
    public: true,
    build: { hash: builds[1].hash!, name: builds[1].name },
    host: 1,
    you,
    slots: [
      {
        slot: 1,
        open: true,
        player: { displayName: "Sarah", connectCode: "SARA#123", controller: "gc", connected: true },
      },
      {
        slot: 2,
        open: true,
        player: { displayName: "Keiko", connectCode: "KEIK#7", controller: "wiimote", connected: true },
      },
      {
        slot: 3,
        open: true,
        player: { displayName: "A very long display name", connectCode: "LONG#42", controller: "gc", connected: false },
      },
      { slot: 4, open: false },
    ],
  },
  session: {
    phase: "running",
    players: 2,
    epoch: 1,
    peers: [
      { slot: 2, connected: true, rttMs: 38 },
      { slot: 3, connected: false, rttMs: -1 },
    ],
  },
  lastRequest: null,
  pid: 1,
  updatedAt: "2026-10-12T20:00:00Z",
  ...extra,
});

const Frame = (props: Partial<ModsViewProps> & { state: ModsState | null }) => (
  <div style={{ width: 900, height: 640, padding: 16, background: "var(--theme-page-background)" }}>
    <ModsView
      publicRooms={{
        kind: "ready",
        rooms: [
          {
            code: "PLNX",
            build: { hash: builds[1].hash!, name: builds[1].name },
            hostName: "Keiko",
            players: 3,
            open: 4,
          },
          {
            code: "QRHQ",
            build: { hash: "9" + "0".repeat(63), name: "Project M 3.6" },
            hostName: "Dee",
            players: 1,
            open: 2,
          },
        ],
      }}
      onLogIn={noop}
      hashProgress={{}}
      startingBuildId={null}
      error={null}
      controller="gc"
      onControllerChange={noop}
      onAddBuild={noop}
      onRemoveBuild={noop}
      onHost={noop}
      onJoin={noop}
      onRequest={noop}
      onCopyCode={noop}
      {...props}
    />
  </div>
);

export const NoBuilds = () => <Frame state={{ builds: [], running: false, status: null }} />;

export const Builds = () => <Frame state={{ builds, running: false, status: null }} hashProgress={{ rex: 0.37 }} />;

export const Hosting = () => <Frame state={{ builds, running: true, status: room(1) }} />;

export const Member = () => <Frame state={{ builds, running: true, status: room(2) }} controller="wiimote" />;

export const Starting = () => <Frame state={{ builds, running: true, status: null }} />;

export const SessionEnded = () => (
  <Frame
    state={{
      builds,
      running: true,
      status: room(2, {
        session: { phase: "error", error: "Could not join: player 1 does not vouch for this machine" },
      }),
    }}
  />
);

export const LaunchError = () => (
  <Frame state={{ builds, running: false, status: null }} error="Dolphin is already open. Close it first." />
);
