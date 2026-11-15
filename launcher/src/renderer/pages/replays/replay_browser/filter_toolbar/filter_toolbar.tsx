import { css } from "@emotion/react";
import SyncIcon from "@mui/icons-material/Sync";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import debounce from "lodash/debounce";
import React from "react";

import { Checkbox, Dropdown } from "@/components/form";
import { ReplayPlayerSearch } from "@/components/player_search/replay_player_search";
import { useReplayFilter } from "@/lib/hooks/use_replay_filter";
import { useReplayPresenter, useReplays } from "@/lib/hooks/use_replays";
import { useSettings } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { ReplaySortOption, SortDirection } from "@/lib/replay_file_sort";

import { FilterToolbarMessages as Messages } from "./filter_toolbar.messages";
import styles from "./filter_toolbar.module.css";

// The folder, then the player search as in Match history (Sarah's board), then the list's buttons; on two
// lines when narrow (filter_toolbar.module.css).
type FilterToolbarProps = {
  disabled?: boolean;
  /** The replay folders' dropdown, first on the bar. */
  folderSelect?: React.ReactNode;
};

export const FilterToolbar = (props: FilterToolbarProps) => {
  const presenter = useReplayPresenter();
  const { disabled, folderSelect } = props;
  const rootSlpPath = useSettings((store) => store.settings.rootSlpPath);
  const extraSlpPaths = useSettings((store) => store.settings.extraSlpPaths);
  const currentFolder = useReplays((store) => store.currentFolder);
  const storeSearchText = useReplayFilter((store) => store.searchText);
  const storeText = useReplayFilter((store) => store.text);
  const setStoreSearchText = useReplayFilter((store) => store.setSearchText);
  const players = useReplayFilter((store) => store.players);
  const setPlayers = useReplayFilter((store) => store.setPlayers);
  const resets = useReplayFilter((store) => store.resets);
  const sortBy = useReplayFilter((store) => store.sortBy);
  const hideShortGames = useReplayFilter((store) => store.hideShortGames);
  const setHideShortGames = useReplayFilter((store) => store.setHideShortGames);
  const setSortBy = useReplayFilter((store) => store.setSortBy);
  const sortDirection = useReplayFilter((store) => store.sortDirection);
  const setSortDirection = useReplayFilter((store) => store.setSortDirection);
  const [searchText, setSearchText] = React.useState(storeText ?? "");
  const { showError } = useToasts();

  const refresh = React.useCallback(() => {
    presenter.init(rootSlpPath, extraSlpPaths, true, currentFolder).catch(showError);
  }, [presenter, rootSlpPath, extraSlpPaths, currentFolder, showError]);

  const debounceChange = React.useMemo(
    () =>
      debounce((text: string) => {
        setStoreSearchText(text);
      }, 300),
    [setStoreSearchText],
  );

  React.useEffect(() => {
    return () => {
      debounceChange.cancel();
    };
  }, [debounceChange]);

  const setNameFilter = (name: string) => {
    setSearchText(name);
    debounceChange(name);
  };

  // The filter cleared (the empty list's button): the box's text too.
  React.useEffect(() => {
    if (resets > 0) {
      debounceChange.cancel();
      setSearchText("");
    }
  }, [resets, debounceChange]);

  // Reload when sort, hideShortGames, or searchText changes (backend filters)
  // Skip initial mount to avoid double-loading
  const isInitialMount = React.useRef(true);
  React.useEffect(() => {
    if (isInitialMount.current) {
      isInitialMount.current = false;
      return;
    }
    presenter.loadFolder(currentFolder, true).catch(showError);
  }, [sortBy, sortDirection, hideShortGames, storeSearchText]);

  return (
    <div className={styles.bar}>
      <div className={styles.inner}>
        <div className={styles.first}>
          {folderSelect}
          <ReplayPlayerSearch
            className={styles.search}
            value={players}
            onChange={setPlayers}
            text={searchText}
            onTextChange={setNameFilter}
            placeholder={Messages.search()}
          />
          <Tooltip title={Messages.refresh()}>
            <span className={styles.refresh}>
              <IconButton onClick={refresh} disabled={disabled} aria-label={Messages.refresh()} size="small">
                <SyncIcon />
              </IconButton>
            </span>
          </Tooltip>
        </div>
        <div className={styles.second}>
          <Dropdown
            value={{
              key: sortBy,
              dir: sortDirection,
            }}
            options={[
              {
                value: {
                  key: ReplaySortOption.DATE,
                  dir: SortDirection.DESC,
                },
                label: Messages.mostRecent(),
              },
              {
                value: {
                  key: ReplaySortOption.DATE,
                  dir: SortDirection.ASC,
                },
                label: Messages.leastRecent(),
              },
              {
                value: {
                  key: ReplaySortOption.GAME_DURATION,
                  dir: SortDirection.DESC,
                },
                label: Messages.longestGame(),
              },
              {
                value: {
                  key: ReplaySortOption.GAME_DURATION,
                  dir: SortDirection.ASC,
                },
                label: Messages.shortestGame(),
              },
            ]}
            onChange={(val) => {
              setSortBy(val.key);
              setSortDirection(val.dir);
            }}
          />
          <Checkbox
            className={styles.hideShort}
            label={Messages.hideShortGames()}
            checked={hideShortGames}
            onChange={() => setHideShortGames(!hideShortGames)}
            css={css`
              .MuiFormControlLabel-label {
                font-size: 12px;
              }
            `}
          />
        </div>
      </div>
    </div>
  );
};
