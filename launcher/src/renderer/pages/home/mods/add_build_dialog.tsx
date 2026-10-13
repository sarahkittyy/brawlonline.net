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

const DISC_FILTERS = [{ name: "Wii disc images", extensions: ["iso", "rvz", "wbfs", "gcz", "ciso", "wia"] }];
const DOL_FILTERS = [{ name: "Launchers", extensions: ["dol", "elf"] }];
const SD_FILTERS = [{ name: "SD card images", extensions: ["raw", "img"] }];

/** "Add a build" (docs/design/brawl-mods.md §8): a name, the disc, an optional launcher and SD card. */
export const AddBuildDialog = ({
  open,
  defaultDisc,
  onClose,
  onAdd,
}: {
  open: boolean;
  /** The Brawl disc set in Settings. */
  defaultDisc: string | null;
  onClose: () => void;
  onAdd: (input: ModBuildInput) => Promise<void>;
}) => {
  const [name, setName] = useState("");
  const [discPath, setDiscPath] = useState<string | null>(defaultDisc);
  const [launcherPath, setLauncherPath] = useState<string | null>(null);
  const [sdPath, setSdPath] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);

  useEffect(() => {
    if (open) {
      setName("");
      setDiscPath(defaultDisc);
      setLauncherPath(null);
      setSdPath(null);
      setAdding(false);
    }
  }, [open, defaultDisc]);

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
      <DialogTitle>{Messages.addBuildTitle()}</DialogTitle>
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
          {Messages.add()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
