import type { ModBuildInput } from "@dolphin/mods/types";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import TextField from "@mui/material/TextField";
import { useEffect, useState } from "react";

import { PathInput } from "@/components/path_input/path_input";

import { ModsMessages as Messages } from "./mods.messages";

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
};

/** "Add a game" / "Add a mod" / "Edit game" (docs/design/brawl-mods.md §8): a name, the disc, an
 * optional launcher and SD card. */
export const AddBuildDialog = ({
  initial,
  onClose,
  onAdd,
}: {
  /** null: closed. */
  initial: AddBuildInitial | null;
  onClose: () => void;
  onAdd: (input: ModBuildInput) => Promise<void>;
}) => {
  const open = initial !== null;
  const [name, setName] = useState("");
  const [discPath, setDiscPath] = useState<string | null>(null);
  const [launcherPath, setLauncherPath] = useState<string | null>(null);
  const [sdPath, setSdPath] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);

  useEffect(() => {
    if (initial) {
      setName(initial.name);
      setDiscPath(initial.discPath);
      setLauncherPath(initial.launcherPath);
      setSdPath(initial.sdPath);
      setAdding(false);
    }
  }, [initial]);

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
        <div>
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
        <div>
          <div>{Messages.sdCard()}</div>
          <PathInput
            value={sdPath ?? ""}
            placeholder={Messages.sdCardHelp()}
            onSelect={setSdPath}
            options={{ filters: SD_FILTERS }}
            endAdornment={clearButton(sdPath, () => setSdPath(null))}
          />
        </div>
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
