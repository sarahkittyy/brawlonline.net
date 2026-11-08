import type { ModBuildCodes, ModCodeLists } from "@dolphin/mods/types";
import { useState } from "react";

import { CodesButton, CodesChip, CodesDialog } from "./codes_dialog";

export default {
  title: "pages/Home/BrawlMods/Codes",
  argTypes: {},
};

const noop = () => undefined;

const allCharacters = {
  name: "All Characters + Costumes",
  author: "James",
  kind: "gecko" as const,
  lines: ["043EBA84 FFFFFFFF", "043EBA88 F9FFFBFF"],
};
const skipToMenu = {
  name: "Skip to Main Menu, No Saving or Auto Save",
  kind: "gecko" as const,
  lines: ["003EBA4B 00000000", "C2074658 00000009"],
};

const lists: ModCodeLists = {
  own: [{ name: "Infinite Time", kind: "gecko", lines: ["04000000 00000063"], author: "me" }],
  ownFiles: ["C:/Users/me/AppData/Roaming/Dolphin Emulator/GameSettings/STKE08.ini"],
  recommended: [allCharacters, skipToMenu],
  dolphin: [{ name: "Widescreen Fix", kind: "gecko", lines: ["0415C2A0 60000000"] }],
};

const Dialog = ({ locked = false }: { locked?: boolean }) => {
  const [codes, setCodes] = useState<ModBuildCodes>({
    on: [
      { ...allCharacters, source: "recommended" },
      { ...skipToMenu, source: "recommended" },
    ],
    yours: [],
  });
  return (
    <CodesDialog
      open={true}
      game="Tatsunoko vs. Capcom"
      gameId="STKE08"
      codes={codes}
      lists={lists}
      database={{ kind: "idle" }}
      locked={locked}
      onChange={setCodes}
      onLoadDatabase={noop}
      onClose={noop}
    />
  );
};

export const Recommended = () => <Dialog />;
export const Locked = () => <Dialog locked={true} />;

export const ButtonAndChip = () => (
  <div style={{ display: "flex", gap: 16, alignItems: "center", padding: 16 }}>
    <CodesButton names={[allCharacters.name, skipToMenu.name]} onClick={noop} />
    <CodesButton names={[]} onClick={noop} />
    <CodesChip names={[allCharacters.name, skipToMenu.name]} />
  </div>
);
