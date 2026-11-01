/**
 * The room list's "Joins with" (Sarah's board, 2026-11-01: "we should also probably have an option
 * to pick which brawl mod to join with anyway,, esp because i think it's an impossible task to
 * perfectly know which one they intend to use"): under a room, which of the player's games joins
 * it, and a menu of every game that can, best first, each with what joining with it downloads of
 * the room's SD card. A pick is kept for every room of that mod (the room's build hash), or for
 * that room alone.
 */

import type { RoomMatch } from "@dolphin/mods/room_match";
import type { ModBuild, ModRoomBuild, ModSdCardCheck } from "@dolphin/mods/types";
import ArrowDropDownRoundedIcon from "@mui/icons-material/ArrowDropDownRounded";
import CheckRoundedIcon from "@mui/icons-material/CheckRounded";
import Checkbox from "@mui/material/Checkbox";
import Divider from "@mui/material/Divider";
import FormControlLabel from "@mui/material/FormControlLabel";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import { useState } from "react";

import { ModsMessages as Messages } from "./mods.messages";
import styles from "./mods.module.css";

/** A game's card against a room's, as the menu shows it: asked, or the answer. */
export type CardCheckState = ModSdCardCheck | "checking";

/** Over this, joining with a game downloads most of the room's card (mods_view.tsx's question
 * says the same at 50 MB). */
const BIG_CARD_BYTES = 50 * 1024 * 1024;

/** Where a game's card check is kept: the room's card against the game's, as it is now. */
export const cardCheckKey = (roomRoot: string, build: ModBuild) =>
  `${roomRoot}|${build.id}|${build.parts?.sdCard ?? "-"}`;

/** "Joins with REX ▾" under a room the player can join: plain text when only one game can. */
export const JoinsWith = ({
  match,
  onOpen,
  anchorRef,
}: {
  match: RoomMatch;
  onOpen: (anchor: HTMLElement) => void;
  anchorRef: (el: HTMLElement | null) => void;
}) => {
  if (!match.build || !match.joinable || match.discOnly || match.choices.length === 0) {
    return null;
  }
  if (match.choices.length === 1) {
    return (
      <div className={styles.joinsWith}>
        {Messages.joinsWith()} <span className={styles.joinsWithName}>{match.build.name}</span>
      </div>
    );
  }
  const open = (el: HTMLElement) => onOpen(el);
  return (
    <div className={styles.joinsWith}>
      {Messages.joinsWith()}{" "}
      <Tooltip title={Messages.joinsWithTooltip()} enterDelay={600} disableInteractive={true}>
        <span
          ref={anchorRef}
          role="button"
          tabIndex={0}
          aria-haspopup="menu"
          className={clsx(styles.joinsWithSelect, !match.sure && styles.joinsWithUnsure)}
          // Inside the room's row, which joins: the menu opens instead.
          onClick={(e) => {
            e.stopPropagation();
            open(e.currentTarget);
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter" || e.key === " ") {
              e.preventDefault();
              e.stopPropagation();
              open(e.currentTarget);
            }
          }}
        >
          {match.build.name}
          <ArrowDropDownRoundedIcon className={styles.joinsWithCaret} />
        </span>
      </Tooltip>
    </div>
  );
};

/** What joining with a game downloads, under its name and at its right. */
function cardText(
  room: ModRoomBuild,
  build: ModBuild,
  check: CardCheckState | undefined,
): { line: string | null; size: string | null; big: boolean } {
  if (!room.sd || !build.sdPath) {
    return { line: null, size: null, big: false };
  }
  if (build.parts?.sdCard === room.sd) {
    return { line: Messages.joinWithCardSame(), size: Messages.joinWithSizeSame(), big: false };
  }
  if (!check || check === "checking") {
    return { line: Messages.joinWithChecking(), size: "…", big: false };
  }
  if (check.kind === "same") {
    return { line: Messages.joinWithCardSame(), size: Messages.joinWithSizeSame(), big: false };
  }
  if (check.kind === "unknown") {
    return { line: Messages.joinWithCardUnknown(), size: "?", big: false };
  }
  const big = check.bytes > BIG_CARD_BYTES;
  return {
    line: big ? Messages.joinWithCardBig() : Messages.joinWithCardSmall(check.files),
    size: Messages.megabytes(check.bytes),
    big,
  };
}

/** The menu of the games that can join a room. `onPick(build, remember)`: remember keeps it for
 * every room of the room's mod. */
export const JoinWithMenu = ({
  anchor,
  room,
  match,
  checks,
  onPick,
  onClose,
}: {
  anchor: HTMLElement | null;
  room: ModRoomBuild | null;
  match: RoomMatch | null;
  /** By `cardCheckKey`. */
  checks: Record<string, CardCheckState>;
  onPick: (build: ModBuild, remember: boolean) => void;
  onClose: () => void;
}) => {
  const [remember, setRemember] = useState(true);
  const open = anchor !== null && room !== null && match !== null;
  return (
    <Menu
      anchorEl={anchor}
      open={open}
      onClose={onClose}
      anchorOrigin={{ vertical: "bottom", horizontal: "left" }}
      transformOrigin={{ vertical: "top", horizontal: "left" }}
      slotProps={{ paper: { className: styles.joinWithPaper } }}
      MenuListProps={{ "aria-label": Messages.joinWithTitle(), dense: true }}
    >
      <li className={styles.joinWithHead}>{Messages.joinWithTitle()}</li>
      {open &&
        match.choices.map((build) => {
          const card = cardText(room, build, room.sd ? checks[cardCheckKey(room.sd, build)] : undefined);
          const picked = build.id === match.build?.id;
          return (
            <MenuItem
              key={build.id}
              selected={picked}
              className={styles.joinWithItem}
              data-build={build.id}
              onClick={() => onPick(build, remember)}
            >
              <span className={styles.joinWithTick}>{picked && <CheckRoundedIcon fontSize="small" />}</span>
              <span className={styles.joinWithText}>
                <span className={styles.joinWithName}>
                  {build.name}
                  {build.hash === room.hash && <span className={styles.joinWithBadge}>{Messages.sameModBadge()}</span>}
                  {build.builtIn && (
                    <span className={clsx(styles.joinWithBadge, styles.joinWithBadgeGrey)}>
                      {Messages.builtInBadge()}
                    </span>
                  )}
                </span>
                <span className={styles.joinWithWhat}>
                  {build.hash === room.hash ? Messages.joinWithSameMod() : Messages.joinWithOtherMod()}
                  {card.line && ` ${card.line}`}
                </span>
              </span>
              <span className={clsx(styles.joinWithSize, card.big && styles.joinWithSizeBig)}>{card.size}</span>
            </MenuItem>
          );
        })}
      <Divider className={styles.joinWithDivider} />
      <li className={styles.joinWithRemember}>
        <FormControlLabel
          control={<Checkbox size="small" checked={remember} onChange={(e) => setRemember(e.target.checked)} />}
          label={<span className={styles.joinWithRememberLabel}>{Messages.joinWithRemember()}</span>}
        />
      </li>
    </Menu>
  );
};
