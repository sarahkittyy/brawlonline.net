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

type ViewState = Omit<ModsState, "controller" | "controllers" | "lastError" | "hashing"> & Partial<ModsState>;

const Frame = ({ state, ...props }: Omit<Partial<ModsViewProps>, "state"> & { state: ViewState | null }) => (
  <div style={{ width: 900, height: 640, display: "flex", background: "var(--theme-page-background)" }}>
    <ModsView
      state={
        state && {
          controller: "gc",
          lastError: null,
          hashing: {},
          controllers: {
            gc: [{ index: 1, device: "GameCube adapter", label: "GameCube adapter" }],
            wiimote: [{ index: 1, device: "", label: "Real Wii Remote", real: true }],
          },
          ...state,
        }
      }
      publicRooms={{
        kind: "ready",
        rooms: [
          {
            code: "PLNX",
            build: { hash: builds[1].hash!, name: builds[1].name },
            hostName: "Keiko",
            players: 3,
            open: 4,
            name: "REX nights",
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
      onControllerChange={noop}
      onConfigureControllers={noop}
      onRefreshControllers={noop}
      onAddBuild={noop}
      onAddFolder={noop}
      onEditBuild={noop}
      onRemoveBuild={noop}
      onHost={noop}
      onJoin={noop}
      onJoinCode={noop}
      onMissingGame={noop}
      onFixBuild={noop}
      onRequest={noop}
      onCopyCode={noop}
      {...props}
    />
  </div>
);

export const NoBuilds = () => <Frame state={{ builds: [], running: false, status: null }} />;

export const Builds = () => <Frame state={{ builds, running: false, status: null }} hashProgress={{ rex: 0.37 }} />;

/** Long names and paths: they get ellipses, the pencil and bin stay in view (no sideways scroll). */
export const LongNames = () => (
  <Frame
    state={{
      builds: [
        {
          ...builds[1],
          id: "long",
          name: "Brawl Minus 5.1 Netplay Edition with every costume pack and the stage expansion",
          discPath:
            "C:/Users/someone/Documents/Dolphin Games/Wii/Super Smash Bros. Brawl (USA) (Rev 2) (Netplay copy for mods).iso",
          launcherPath: "C:/Users/someone/Documents/Dolphin Games/Mods/Brawl Minus 5.1/Launcher/BrawlMinusLauncher.dol",
        },
        ...builds,
      ],
      running: false,
      status: null,
    }}
  />
);

/** DolphinTool hashing a compressed disc: no share to show until it is done. */
export const CheckingCompressedDisc = () => (
  <Frame state={{ builds, running: false, status: null }} hashProgress={{ rex: null }} />
);

export const Hosting = () => <Frame state={{ builds, running: true, status: room(1) }} />;

/** The host's room named, with the pencil. */
export const HostingNamed = () => (
  <Frame
    state={{ builds, running: true, status: room(1) }}
    yourRoom={{ kind: "mods", code: room(1).room!.code, host: true, name: "sunday NSMB races, all welcome!!" }}
    onRenameRoom={() => Promise.resolve()}
  />
);

export const Member = () => <Frame state={{ builds, running: true, status: room(2) }} />;

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

/** A room's parts (docs/design/brawl-mods.md §6.5): the same disc and card (green), another SD card
 * (amber: joinable, downloaded at the join), another disc (red, greyed out). */
const DISC = "d41e6cba05c2d1a2bb3f4cbb17a8e7f9d6c01a2b";
const REX_CARD = "5d1a90c3e2f4b6a8c0d2e4f6a8b0c2d4e6f8a0b2c4d6e8f0a2b4c6d8e0f2a4b6";
const rexBuild: ModBuild = {
  id: "rex",
  name: "REX",
  discPath: "C:/Games/Super Smash Bros. Brawl (USA) (Rev 1).iso",
  launcherPath: "C:/Games/REX/user/Launcher/REX Netplay Launcher.elf",
  sdPath: "C:/Games/REX/user/Wii/sd.raw",
  gameId: "RSBE01",
  revision: 1,
  hash: "aa21d0c37aa00000000000000000000000000000000000000000000000000002",
  parts: { disc: DISC, launcher: "b".repeat(64), sd: true, sdCard: REX_CARD },
  hashKey: "k",
};

export const RoomParts = () => (
  <Frame
    state={{ builds: [rexBuild, ...builds], running: false, status: null }}
    publicRooms={{
      kind: "ready",
      rooms: [
        {
          code: "SAME",
          build: { hash: rexBuild.hash!, name: "REX", disc: DISC, discLabel: "RSBE01 Rev 1", sd: REX_CARD },
          hostName: "Keiko",
          players: 2,
          open: 4,
        },
        {
          code: "SDSD",
          build: {
            hash: "c".repeat(64),
            name: "REX",
            disc: DISC,
            discLabel: "RSBE01 Rev 1",
            sd: "9e0c7b2a4d6f8e1c3a5b7d9f0e2c4a6b8d0f2e4c6a8b0d2f4e6c8a0b2d4f6e8a",
          },
          hostName: "SARA#694",
          players: 1,
          open: 4,
        },
        {
          code: "REV2",
          build: {
            hash: "d".repeat(64),
            name: "REX",
            disc: "7c03aa9e11b2c3d4e5f60718293a4b5c6d7e8f90",
            discLabel: "RSBE01 Rev 2",
            sd: REX_CARD,
          },
          hostName: "JoeBudson",
          players: 1,
          open: 2,
        },
      ],
    }}
  />
);

const syncing = (sd_sync: NonNullable<ModRoomStatus["session"]["sd_sync"]>): ModRoomStatus =>
  room(2, { session: { phase: "joining", sd_sync } });

/** A joiner whose SD card differs from the host's: the download needs its consent. */
export const SdCardDialog = () => (
  <Frame
    state={{
      builds: [rexBuild],
      running: true,
      status: syncing({
        step: "ask",
        checked: 1,
        bytes: 280 * 1024 * 1024,
        files: 412,
        names: [
          "REX_/PF/Fighter/HERO/FitHeroMotionEtc.pac",
          "REX_/PF/Fighter/NU/FitNuMotion.pac",
          "REX_/PF/SOUND/STRM/RX_Battlefield.brstm",
          "REX_/PF/Fighter/PICHU/FitPichu.pac",
        ],
        eta_s: 240,
      }),
    }}
  />
);

export const SdCardDownloading = () => (
  <Frame
    state={{
      builds: [rexBuild],
      running: true,
      status: syncing({
        step: "downloading",
        checked: 1,
        bytes: 280 * 1024 * 1024,
        files: 412,
        names: [],
        eta_s: 150,
        rate: 1.1 * 1024 * 1024,
        done: 112 * 1024 * 1024,
        total: 280 * 1024 * 1024,
      }),
    }}
  />
);
