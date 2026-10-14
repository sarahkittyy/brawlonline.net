import type { DolphinControllers, ModLocalInput, ModsController } from "@dolphin/mods/types";
import SportsEsportsOutlinedIcon from "@mui/icons-material/SportsEsportsOutlined";
import Divider from "@mui/material/Divider";
import ListItemText from "@mui/material/ListItemText";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import { clsx } from "clsx";

import { ModsMessages as Messages } from "./mods.messages";
import styles from "./mods.module.css";

const CONFIGURE = "configure";

const kindName = (kind: ModsController) => (kind === "wiimote" ? Messages.wiiRemote() : Messages.gameCube());

/** What is set up in Dolphin for a kind, folded: "Xbox controller, GameCube adapter". */
function setUpLabel(controllers: DolphinControllers, kind: ModsController): string {
  const labels = [...new Set(controllers[kind].map((c) => c.label))];
  return labels.length > 0 ? labels.join(", ") : Messages.notSetUp();
}

/** The label of the controller Dolphin plays, from what is set up ("Xbox controller"). */
function setUpDeviceLabel(controllers: DolphinControllers, input: ModLocalInput): string {
  const set = controllers[input.controller].find((c) => c.index === input.index);
  return set?.label ?? input.device;
}

/**
 * The controller menu (Home > Other Games): a GameCube controller or a Wii Remote, each the
 * first one plugged in of the ones set up in Dolphin, and Configure (Dolphin's Controllers
 * settings). It works mid-game: the switch is the room's at the next frame.
 */
export const ControllerMenu = ({
  controller,
  controllers,
  input,
  gameWants,
  onChange,
  onConfigure,
  onOpen,
}: {
  controller: ModsController;
  controllers: DolphinControllers;
  /** What Dolphin plays with now, while a room runs. */
  input: ModLocalInput | undefined;
  /** What the running game is made for, if it is known. */
  gameWants: ModsController | null;
  onChange: (controller: ModsController) => void;
  onConfigure: () => void;
  /** The menu opens: Dolphin's settings are read again. */
  onOpen: () => void;
}) => {
  let note: string | null = null;
  let warn = false;
  if (input) {
    const kind = kindName(input.controller);
    if (!input.device) {
      note = Messages.noneSetUp(kind);
      warn = true;
    } else if (!input.plugged) {
      note = Messages.notPluggedIn(kind);
      warn = true;
    } else {
      note = Messages.playingWith(input.real ? Messages.realWiiRemote() : setUpDeviceLabel(controllers, input));
    }
  }
  if (gameWants === "wiimote" && controller === "gc") {
    note = Messages.gameNeedsWiiRemote();
    warn = true;
  }
  return (
    <div className={styles.controllerRow}>
      <span className={styles.controllerLabel}>{Messages.controller()}</span>
      <Select
        size="small"
        value={controller}
        onOpen={onOpen}
        inputProps={{ "aria-label": Messages.controller() }}
        renderValue={(value) => (
          <span className={styles.controllerValue}>
            <SportsEsportsOutlinedIcon fontSize="small" />
            {kindName(value as ModsController)}
          </span>
        )}
        onChange={(e) => {
          const value = e.target.value;
          if (value === CONFIGURE) {
            onConfigure();
          } else {
            onChange(value as ModsController);
          }
        }}
      >
        <MenuItem value="gc">
          <ListItemText primary={Messages.gameCube()} secondary={setUpLabel(controllers, "gc")} />
        </MenuItem>
        <MenuItem value="wiimote">
          <ListItemText primary={Messages.wiiRemote()} secondary={setUpLabel(controllers, "wiimote")} />
        </MenuItem>
        <Divider />
        <MenuItem value={CONFIGURE}>
          <ListItemText primary={<em>{Messages.configure()}</em>} />
        </MenuItem>
      </Select>
      {note && <span className={clsx(styles.controllerNote, warn && styles.controllerWarn)}>{note}</span>}
    </div>
  );
};
