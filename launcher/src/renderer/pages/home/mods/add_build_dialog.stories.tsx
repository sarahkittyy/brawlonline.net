import type { AddBuildInitial, SaveSectionProps } from "./add_build_dialog";
import { AddBuildDialog } from "./add_build_dialog";

export default {
  title: "pages/Home/BrawlMods/EditGame",
  argTypes: {},
};

const noop = () => undefined;

const nsmbw: AddBuildInitial = {
  kind: "edit",
  name: "",
  discPath: "C:/Users/me/Saved Games/Dolphin-x64/Games/New Super Mario Bros. Wii (USA) (En,Fr,Es) (Rev 2).iso",
  launcherPath: null,
  sdPath: null,
};

const folder =
  "C:/Users/me/AppData/Roaming/Brawl Online/netplay/User/Saves/14e34197d0329e2e/title/00010000/534d4e45/data";

const save = (info: SaveSectionProps["info"]): SaveSectionProps => ({
  info,
  busy: false,
  onImport: noop,
  onOpenFolder: noop,
});

const Edit = ({ section }: { section: SaveSectionProps | null }) => (
  <AddBuildDialog initial={nsmbw} onClose={noop} onAdd={async () => undefined} save={section} />
);

export const WithSave = () => (
  <Edit section={save({ folder, files: 2, bytes: 31_000, modified: "2026-10-19T02:23:07.000Z" })} />
);

export const NoSaveYet = () => <Edit section={save({ folder, files: 0, bytes: 0, modified: null })} />;

export const NotCheckedYet = () => <Edit section={save("not-ready")} />;

export const GameCube = () => (
  <Edit
    section={{
      ...save({
        folder: "C:/Users/me/AppData/Roaming/Brawl Online/netplay/User/Saves/14e34197d0329e2e/GC",
        files: 1,
        bytes: 90_176,
        modified: "2026-10-23T02:50:07.000Z",
      }),
      gamecube: true,
    }}
  />
);
