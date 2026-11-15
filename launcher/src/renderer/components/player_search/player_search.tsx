import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import SearchIcon from "@mui/icons-material/Search";
import Autocomplete from "@mui/material/Autocomplete";
import Chip from "@mui/material/Chip";
import InputAdornment from "@mui/material/InputAdornment";
import TextField from "@mui/material/TextField";
import type React from "react";
import { useState } from "react";

import styles from "./player_search.module.css";

type PlayerSearchProps<T> = {
  /** The players picked: a tag each (their code and an X). */
  value: T[];
  onChange: (players: T[]) => void;
  /** Suggestions for `text` (the caller finds them). */
  options: T[];
  loading?: boolean;
  /** What is typed (lists of this computer's replays also filter by it as it is typed). */
  text: string;
  onTextChange: (text: string) => void;
  getKey: (p: T) => string;
  getCode: (p: T) => string;
  getName: (p: T) => string;
  /** The picture on a suggestion's line (22 px). */
  renderPicture: (p: T) => React.ReactNode;
  placeholder: string;
  /** Tags at most. */
  max?: number;
  testId?: string;
  className?: string;
};

/**
 * The player search of Match history, Replays and Highlights (Sarah's board): typing suggests players (their picture,
 * name and code); a pick becomes a tag with only the code and an X, and the list shows what every tagged player is
 * in. Backspace in the empty box removes the last tag.
 */
export const PlayerSearch = <T,>({
  value,
  onChange,
  options,
  loading,
  text,
  onTextChange,
  getKey,
  getCode,
  getName,
  renderPicture,
  placeholder,
  max = 4,
  testId = "player-filter",
  className,
}: PlayerSearchProps<T>) => {
  const [focused, setFocused] = useState(false);
  const full = value.length >= max;
  const shown = full ? [] : options;
  return (
    <Autocomplete
      multiple={true}
      size="small"
      forcePopupIcon={false}
      // Open only with suggestions to show (no "No options" box of MUI's own words).
      open={focused && shown.length > 0}
      onOpen={() => setFocused(true)}
      onClose={() => setFocused(false)}
      className={`${styles.filter} ${className ?? ""}`}
      options={shown}
      value={value}
      inputValue={text}
      loading={loading}
      filterOptions={(o) => o}
      filterSelectedOptions={true}
      isOptionEqualToValue={(a, b) => getKey(a) === getKey(b)}
      getOptionLabel={getCode}
      onInputChange={(_, v, reason) => {
        if (reason !== "reset") {
          onTextChange(v);
        }
      }}
      onChange={(_, players) => {
        onChange(players.slice(0, max));
        onTextChange("");
      }}
      renderOption={(props, p) => (
        <li {...props} key={getKey(p)} className={`${props.className ?? ""} ${styles.option}`}>
          {renderPicture(p)}
          <span className={styles.name}>{getName(p)}</span>
          <span className={styles.code}>{getCode(p)}</span>
        </li>
      )}
      renderTags={(players, getTagProps) =>
        players.map((p, i) => {
          const { key, ...rest } = getTagProps({ index: i });
          return (
            <Chip
              key={key}
              {...rest}
              size="small"
              label={getCode(p)}
              deleteIcon={<CloseRoundedIcon />}
              className={styles.tag}
              data-testid={`${testId}-tag-${getCode(p)}`}
            />
          );
        })
      }
      renderInput={(params) => (
        <TextField
          {...params}
          hiddenLabel={true}
          placeholder={value.length === 0 ? placeholder : undefined}
          inputProps={{ ...params.inputProps, "aria-label": placeholder, "data-testid": testId }}
          InputProps={{
            ...params.InputProps,
            startAdornment: (
              <>
                <InputAdornment position="start">
                  <SearchIcon fontSize="small" />
                </InputAdornment>
                {params.InputProps.startAdornment}
              </>
            ),
          }}
        />
      )}
    />
  );
};
