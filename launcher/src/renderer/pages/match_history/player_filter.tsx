import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import SearchIcon from "@mui/icons-material/Search";
import Autocomplete from "@mui/material/Autocomplete";
import Chip from "@mui/material/Chip";
import InputAdornment from "@mui/material/InputAdornment";
import TextField from "@mui/material/TextField";
import { useQuery } from "@tanstack/react-query";
import { useState } from "react";

import { MainPicture } from "@/components/main_picture/main_picture";
import { useStockIcon } from "@/components/profile/character_icon";
import { useSearchText } from "@/lib/hooks/use_search_text";
import { SpectateMessages } from "@/pages/home/spectate/spectate.messages";
import { useServices } from "@/services";
import type { MatchOpponent } from "@/services/backend/types";

import styles from "./player_filter.module.css";

/** Players filtered by at once (the server's limit). */
const MAX_PLAYERS = 4;

/**
 * Match history's player filter (Sarah's board): typing suggests the players the player has played with (their main,
 * name and code); a pick becomes a tag with only the code and an X, and the history shows the matches every tagged
 * player played in. Backspace in the empty box removes the last tag.
 */
export const PlayerFilter = ({
  value,
  onChange,
}: {
  value: MatchOpponent[];
  onChange: (players: MatchOpponent[]) => void;
}) => {
  const { backendService } = useServices();
  const [text, setText] = useState("");
  const [focused, setFocused] = useState(false);
  const search = useSearchText(text);
  const full = value.length >= MAX_PLAYERS;
  const { data, isFetching } = useQuery({
    queryKey: ["matchOpponents", search],
    queryFn: () => backendService.fetchOpponents(search),
    enabled: !full,
    staleTime: 30_000,
  });
  return (
    <Autocomplete
      multiple={true}
      size="small"
      forcePopupIcon={false}
      // Open only with suggestions to show (no "No options" box of MUI's own words).
      open={focused && !full && (data?.length ?? 0) > 0}
      onOpen={() => setFocused(true)}
      onClose={() => setFocused(false)}
      className={styles.filter}
      options={full ? [] : data ?? []}
      value={value}
      inputValue={text}
      loading={isFetching}
      filterOptions={(options) => options}
      filterSelectedOptions={true}
      isOptionEqualToValue={(a, b) => a.uid === b.uid}
      getOptionLabel={(p) => p.connectCode}
      onInputChange={(_, v, reason) => {
        if (reason !== "reset") {
          setText(v);
        }
      }}
      onChange={(_, players) => {
        onChange(players.slice(0, MAX_PLAYERS));
        setText("");
      }}
      renderOption={(props, p) => (
        <li {...props} key={p.uid} className={`${props.className ?? ""} ${styles.option}`}>
          <Picture main={p.main} />
          <span className={styles.name}>{p.displayName}</span>
          <span className={styles.code}>{p.connectCode}</span>
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
              label={p.connectCode}
              deleteIcon={<CloseRoundedIcon />}
              className={styles.tag}
              data-testid={`player-filter-tag-${p.connectCode}`}
            />
          );
        })
      }
      renderInput={(params) => (
        <TextField
          {...params}
          hiddenLabel={true}
          placeholder={value.length === 0 ? SpectateMessages.search() : undefined}
          inputProps={{ ...params.inputProps, "aria-label": SpectateMessages.search(), "data-testid": "player-filter" }}
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

const Picture = ({ main }: { main?: string | null }) => {
  const url = useStockIcon(main ?? "");
  return <MainPicture url={main ? url ?? null : null} size={22} />;
};
