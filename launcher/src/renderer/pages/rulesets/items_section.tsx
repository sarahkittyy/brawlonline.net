import type { ItemSwitchEntry } from "@common/items";
import {
  ITEM_FREQUENCIES,
  ITEM_SWITCH_ITEMS,
  ITEM_SWITCH_SETTINGS,
  itemOn,
  itemsOn,
  itemsSummary,
  withAllItems,
  withItem,
} from "@common/items";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import Button from "@mui/material/Button";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import { useState } from "react";

import { assetUrl, useGameAssets } from "@/styles/game_theme";

import { ItemsMessages as Messages } from "./items_section.messages";

/** P+'s BOMB RAIN frequency (BombRain.asm): sudden death's Bob-omb drops. */
const BOMB_RAIN = 6;
import styles from "./items_section.module.css";

/** "Off", "All items · Medium", "12 items · Low" (frequency None or no item on: off, as P+ plays it). */
export function itemsLine(frequency: number, itemSwitch: readonly number[]): string {
  const s = itemsSummary(frequency, itemSwitch);
  const freq = ITEM_FREQUENCIES[frequency] ?? String(frequency);
  if (frequency === BOMB_RAIN) {
    return freq; // (sudden death's bombs, whatever the item switch)
  }
  if (s === "none") {
    return Messages.off();
  }
  return s === "all" ? Messages.allItems(freq) : Messages.someItems(s, freq);
}

const useItemIcon = () => {
  const icons = useGameAssets((s) => s.state?.manifest?.items);
  return (bit: number): string | undefined => {
    const e = icons?.[String(bit)];
    return e ? assetUrl(e.file) : undefined;
  };
};

const ItemCell = ({
  entry,
  on,
  icon,
  onClick,
}: {
  entry: ItemSwitchEntry;
  on: boolean;
  icon?: string;
  onClick?: () => void;
}) => {
  const body = (
    <>
      {icon ? (
        <img className={styles.icon} src={icon} alt="" draggable={false} />
      ) : (
        <span className={styles.iconPlaceholder} />
      )}
      <span className={styles.name}>{entry.name}</span>
      <span className={clsx(styles.state, on ? styles.stateOn : styles.stateOff)}>
        {on ? Messages.on() : Messages.off()}
      </span>
    </>
  );
  return onClick ? (
    <Tooltip title={entry.category} enterDelay={600} disableInteractive={true}>
      <button
        type="button"
        className={clsx(styles.cell, on ? styles.on : styles.off)}
        aria-pressed={on}
        data-item={entry.bit}
        onClick={onClick}
      >
        {body}
      </button>
    </Tooltip>
  ) : (
    <div className={clsx(styles.cell, styles.readOnly, on ? styles.on : styles.off)} data-item={entry.bit}>
      {body}
    </div>
  );
};

/**
 * The rules' items as P+'s ITEM SWITCH shows them (src/common/items.ts): the frequency, ALL ON / ALL OFF and every
 * item with its icon from the game files, then P+'s item settings (Stage Items, EX Items...). Collapsed to a summary
 * line until opened (`bare`: always open, without its header: the editor's Items tab). Without `onChange` it is
 * read-only.
 */
export const ItemsSection = ({
  frequency,
  itemSwitch,
  onChange,
  defaultOpen = false,
  bare = false,
}: {
  frequency: number;
  itemSwitch: readonly number[];
  onChange?: (frequency: number, itemSwitch: number[]) => void;
  defaultOpen?: boolean;
  bare?: boolean;
}) => {
  const [toggled, setOpen] = useState(defaultOpen);
  const open = bare || toggled;
  const icon = useItemIcon();
  const editable = onChange !== undefined;
  const set = (sw: number[]) => onChange?.(frequency, sw);

  return (
    <div className={styles.section} data-testid="items-section">
      {!bare && (
        <button
          type="button"
          className={styles.header}
          aria-expanded={open}
          onClick={() => setOpen(!open)}
          data-testid="items-toggle"
        >
          <span className={styles.headerTitle}>{Messages.items()}</span>
          <span className={styles.headerSummary}>{itemsLine(frequency, itemSwitch)}</span>
          <ExpandMoreIcon className={clsx(styles.chevron, open && styles.chevronOpen)} fontSize="small" />
        </button>
      )}
      {open && (
        <div className={styles.body}>
          <div className={styles.controls}>
            <span className={styles.label}>{Messages.frequency()}</span>
            {editable ? (
              <ToggleButtonGroup
                size="small"
                exclusive={true}
                value={String(frequency)}
                onChange={(_, v: string | null) => v !== null && onChange?.(Number(v), [...itemSwitch])}
              >
                {ITEM_FREQUENCIES.map((f, i) => (
                  <ToggleButton key={f} value={String(i)} sx={{ textTransform: "none", py: "2px", px: 1.25 }}>
                    {f}
                  </ToggleButton>
                ))}
              </ToggleButtonGroup>
            ) : (
              <span>{ITEM_FREQUENCIES[frequency] ?? frequency}</span>
            )}
            {editable && (
              <span className={styles.allButtons}>
                <Button size="small" color="secondary" onClick={() => set(withAllItems(itemSwitch, true))}>
                  {Messages.allOn()}
                </Button>
                <Button size="small" color="secondary" onClick={() => set(withAllItems(itemSwitch, false))}>
                  {Messages.allOff()}
                </Button>
              </span>
            )}
          </div>
          {frequency > 0 && frequency !== BOMB_RAIN && itemsOn(itemSwitch).length === 0 && (
            <p className={styles.note}>{Messages.noneOn()}</p>
          )}
          {frequency === BOMB_RAIN && <p className={styles.note}>{Messages.bombRain()}</p>}
          {frequency === 0 && itemsOn(itemSwitch).length > 0 && (
            <p className={styles.note}>{Messages.frequencyNone()}</p>
          )}
          <div className={styles.grid}>
            {ITEM_SWITCH_ITEMS.map((e) => (
              <ItemCell
                key={e.bit}
                entry={e}
                on={itemOn(itemSwitch, e.bit)}
                icon={icon(e.bit)}
                onClick={editable ? () => set(withItem(itemSwitch, e.bit, !itemOn(itemSwitch, e.bit))) : undefined}
              />
            ))}
          </div>
          <span className={styles.label}>{Messages.settings()}</span>
          <div className={styles.grid}>
            {ITEM_SWITCH_SETTINGS.map((e) => (
              <ItemCell
                key={e.bit}
                entry={e}
                on={itemOn(itemSwitch, e.bit)}
                icon={icon(e.bit)}
                onClick={editable ? () => set(withItem(itemSwitch, e.bit, !itemOn(itemSwitch, e.bit))) : undefined}
              />
            ))}
          </div>
        </div>
      )}
    </div>
  );
};
