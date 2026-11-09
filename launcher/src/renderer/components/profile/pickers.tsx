import { CHARACTERS, COUNTRIES, MAX_MAINS } from "@common/profile";
import StarRoundedIcon from "@mui/icons-material/StarRounded";
import Button from "@mui/material/Button";
import { clsx } from "clsx";
import { useMemo, useState } from "react";

import { Flag, NoFlag } from "@/components/flag/flag";

import { CharacterIcon, useHasStockIcons } from "./character_icon";
import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";

/**
 * Picks mains: every character's stock icon in a grid (no disc: name tags, wrapping). A click selects or drops one;
 * the selected ones have a bright rounded outline. The primary is the first picked until another is double-clicked
 * (which also picks it); it has a gold outline and a star. Dropping the primary makes the next one primary. At
 * MAX_MAINS the rest can't be picked. OK hands the list back, the primary first.
 */
export const MainsPicker = ({ initial, onDone }: { initial: string[]; onDone: (mains: string[]) => void }) => {
  const [picked, setPicked] = useState<string[]>(initial);
  const icons = useHasStockIcons();
  const full = picked.length >= MAX_MAINS;

  const toggle = (key: string) =>
    setPicked((p) => (p.includes(key) ? p.filter((k) => k !== key) : p.length >= MAX_MAINS ? p : [...p, key]));
  const makePrimary = (key: string) =>
    setPicked((p) => (p.includes(key) || p.length < MAX_MAINS ? [key, ...p.filter((k) => k !== key)] : p));

  return (
    <div className={styles.picker} data-testid="mains-picker">
      <div className={icons ? styles.iconGrid : styles.tagGrid}>
        {CHARACTERS.map(({ key, name }) => {
          const selected = picked.includes(key);
          const primary = picked[0] === key;
          return (
            <button
              key={key}
              type="button"
              className={clsx(styles.choice, selected && styles.selected, primary && styles.primary)}
              aria-pressed={selected}
              aria-label={name}
              disabled={!selected && full}
              onClick={() => toggle(key)}
              onDoubleClick={() => makePrimary(key)}
              data-testid={`pick-${key}`}
            >
              <CharacterIcon character={key} size={32} forceTag={!icons} />
              {primary && <StarRoundedIcon className={styles.star} />}
            </button>
          );
        })}
      </div>
      <div className={styles.pickerFooter}>
        <span className={styles.hint}>
          {Messages.primaryHint()}
          {full && ` ${Messages.mainsFull(MAX_MAINS)}`}
        </span>
        <Button variant="contained" size="small" onClick={() => onDone(picked)}>
          {Messages.ok()}
        </Button>
      </div>
    </div>
  );
};

/**
 * Picks a country: a search box over a grid of every flag (its name on hover and in the search), the dashed "no
 * country" first. A click picks one and closes.
 */
export const CountryPicker = ({
  current,
  onPick,
}: {
  current: string | null;
  onPick: (code: string | null) => void;
}) => {
  const [query, setQuery] = useState("");
  const shown = useMemo(() => {
    const q = query.trim().toLowerCase();
    return q ? COUNTRIES.filter((c) => c.name.toLowerCase().includes(q) || c.code === q) : COUNTRIES;
  }, [query]);
  return (
    <div className={styles.picker} data-testid="country-picker">
      <input
        className={styles.search}
        value={query}
        placeholder={Messages.searchCountries()}
        aria-label={Messages.searchCountries()}
        autoFocus={true}
        spellCheck={false}
        onChange={(e) => setQuery(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter" && shown.length === 1) {
            onPick(shown[0].code);
          }
        }}
      />
      <div className={styles.flagGrid}>
        <button
          type="button"
          className={clsx(styles.choice, current == null && styles.selected)}
          title={Messages.noCountry()}
          aria-label={Messages.noCountry()}
          onClick={() => onPick(null)}
          data-testid="pick-no-country"
        >
          <NoFlag height={21} />
        </button>
        {shown.map((c) => (
          <button
            key={c.code}
            type="button"
            className={clsx(styles.choice, current === c.code && styles.selected)}
            title={c.name}
            aria-label={c.name}
            onClick={() => onPick(c.code)}
            data-testid={`pick-country-${c.code}`}
          >
            <Flag code={c.code} height={21} />
          </button>
        ))}
      </div>
      {shown.length === 0 && <div className={styles.hint}>{Messages.noCountryMatch(query.trim())}</div>}
    </div>
  );
};
