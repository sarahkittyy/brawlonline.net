import type { ModBuildInput, ModSaveInfo } from "@dolphin/mods/types";
import ArrowDropDownRoundedIcon from "@mui/icons-material/ArrowDropDownRounded";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Divider from "@mui/material/Divider";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import TextField from "@mui/material/TextField";
import { useEffect, useState } from "react";

import { PathInput } from "@/components/path_input/path_input";

import { ModsMessages as Messages } from "./mods.messages";
import styles from "./mods.module.css";

const DISC_FILTERS = [
  { name: "Wii and GameCube disc images", extensions: ["iso", "rvz", "wbfs", "gcz", "ciso", "wia", "gcm", "nkit.iso"] },
];
const DOL_FILTERS = [{ name: "Launchers", extensions: ["dol", "elf"] }];
const SD_FILTERS = [{ name: "SD card images", extensions: ["raw", "img"] }];

/** What the dialog opens with: a game (a disc), a mod found in its Dolphin folder (on the Brawl
 * disc set in Settings), or a game in the list being edited (its pencil). */
export type AddBuildInitial = {
  kind: "game" | "mod" | "edit";
  name: string;
  discPath: string | null;
  launcherPath: string | null;
  sdPath: string | null;
  /** The field a room pointed at (its disc or SD card differs from the room's): it pulses gold
   * until it is clicked. */
  highlight?: "disc" | "sd";
};

/** The edit window's save data (`save_import.ts`, a GameCube game's `gc_save.ts`): what rooms play,
 * imported from a data.bin (a .gci or a memory card) or a Dolphin folder, and its folder. */
export type SaveSectionProps = {
  /** A GameCube game: its memory card's saves. */
  gamecube?: boolean;
  /** "loading", "not-ready" (the game is not hashed yet: its store is named by its hash), or the
   * save as it is. */
  info: ModSaveInfo | "loading" | "not-ready";
  busy: boolean;
  onImport: (from: "file" | "folder") => void;
  onOpenFolder: () => void;
};

export const formatSaveSummary = (info: ModSaveInfo) =>
  Messages.saveSummary(info.files, info.modified ? new Date(info.modified).toLocaleString() : "");

const SaveSection = ({ gamecube, info, busy, onImport, onOpenFolder }: SaveSectionProps) => {
  const [menuAnchor, setMenuAnchor] = useState<HTMLElement | null>(null);
  const ready = info !== "loading" && info !== "not-ready";
  let line: string;
  if (info === "loading") {
    line = Messages.saveLoading();
  } else if (info === "not-ready") {
    line = Messages.saveNotReady();
  } else {
    line = info.files > 0 ? formatSaveSummary(info) : Messages.saveNone();
  }
  const pick = (from: "file" | "folder") => {
    setMenuAnchor(null);
    onImport(from);
  };
  return (
    <div>
      <div>{Messages.saveData()}</div>
      <div className={styles.saveLine}>{line}</div>
      <div className={styles.saveHelp}>{Messages.saveHelp()}</div>
      <div className={styles.saveButtons}>
        <Button
          variant="outlined"
          size="small"
          disabled={!ready || busy}
          endIcon={<ArrowDropDownRoundedIcon />}
          onClick={(e) => setMenuAnchor(e.currentTarget)}
        >
          {Messages.importSave()}
        </Button>
        <Button variant="outlined" size="small" disabled={!ready || busy} onClick={onOpenFolder}>
          {Messages.openSaveLocation()}
        </Button>
      </div>
      <Menu anchorEl={menuAnchor} open={menuAnchor !== null} onClose={() => setMenuAnchor(null)}>
        <MenuItem onClick={() => pick("file")}>
          {gamecube ? Messages.importGcSaveFile() : Messages.importSaveFile()}
        </MenuItem>
        <MenuItem onClick={() => pick("folder")}>{Messages.importSaveFolder()}</MenuItem>
      </Menu>
    </div>
  );
};

/** "Add a game" / "Add a mod" / "Edit game" (docs/design/brawl-mods.md §8): a name, the disc, an
 * optional launcher and SD card; editing a game, its save data too. */
export const AddBuildDialog = ({
  initial,
  onClose,
  onAdd,
  save,
}: {
  /** null: closed. */
  initial: AddBuildInitial | null;
  onClose: () => void;
  onAdd: (input: ModBuildInput) => Promise<void>;
  /** The edited game's save; absent otherwise. */
  save?: SaveSectionProps | null;
}) => {
  const open = initial !== null;
  const [name, setName] = useState("");
  const [discPath, setDiscPath] = useState<string | null>(null);
  const [launcherPath, setLauncherPath] = useState<string | null>(null);
  const [sdPath, setSdPath] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);
  const [highlight, setHighlight] = useState<"disc" | "sd" | null>(null);

  useEffect(() => {
    if (initial) {
      setName(initial.name);
      setDiscPath(initial.discPath);
      setLauncherPath(initial.launcherPath);
      setSdPath(initial.sdPath);
      setAdding(false);
      setHighlight(initial.highlight ?? null);
    }
  }, [initial]);
  // The field pulses until the player clicks it.
  const pulse = (field: "disc" | "sd") => ({
    className: highlight === field ? styles.fixField : undefined,
    onClickCapture: () => {
      if (highlight === field) {
        setHighlight(null);
      }
    },
  });

  const add = async () => {
    if (!discPath) {
      return;
    }
    setAdding(true);
    try {
      await onAdd({ name, discPath, launcherPath, sdPath });
    } finally {
      setAdding(false);
    }
  };

  const clearButton = (value: string | null, clear: () => void) =>
    value ? (
      <Button size="small" onClick={clear}>
        {Messages.clear()}
      </Button>
    ) : undefined;

  return (
    <Dialog open={open} onClose={onClose} fullWidth={true} maxWidth="sm">
      <DialogTitle>
        {initial?.kind === "edit"
          ? Messages.editBuildTitle()
          : initial?.kind === "mod"
          ? Messages.addModTitle()
          : Messages.addBuildTitle()}
      </DialogTitle>
      <DialogContent sx={{ display: "flex", flexDirection: "column", gap: 2, pt: 1 }}>
        <TextField
          label={Messages.name()}
          placeholder={Messages.namePlaceholder()}
          value={name}
          inputProps={{ maxLength: 64 }}
          onChange={(e) => setName(e.target.value)}
          margin="dense"
        />
        <div {...pulse("disc")}>
          <div>{Messages.disc()}</div>
          <PathInput
            value={discPath ?? ""}
            placeholder={Messages.discHelp()}
            onSelect={setDiscPath}
            options={{ filters: DISC_FILTERS }}
          />
        </div>
        <div>
          <div>{Messages.launcher()}</div>
          <PathInput
            value={launcherPath ?? ""}
            placeholder={Messages.launcherHelp()}
            onSelect={setLauncherPath}
            options={{ filters: DOL_FILTERS }}
            endAdornment={clearButton(launcherPath, () => setLauncherPath(null))}
          />
        </div>
        <div {...pulse("sd")}>
          <div>{Messages.sdCard()}</div>
          <PathInput
            value={sdPath ?? ""}
            placeholder={Messages.sdCardHelp()}
            onSelect={setSdPath}
            options={{ filters: SD_FILTERS }}
            endAdornment={clearButton(sdPath, () => setSdPath(null))}
          />
        </div>
        {initial?.kind === "edit" && save && (
          <>
            <Divider />
            <SaveSection {...save} />
          </>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>{Messages.cancel()}</Button>
        <Button variant="contained" disabled={!discPath || adding} onClick={() => void add()}>
          {initial?.kind === "edit" ? Messages.save() : Messages.add()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
