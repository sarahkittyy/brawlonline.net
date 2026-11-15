import { css } from "@emotion/react";
import styled from "@emotion/styled";
import FolderIcon from "@mui/icons-material/Folder";
import SearchIcon from "@mui/icons-material/Search";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import type { FolderResult } from "@replays/types";
import React from "react";

import { IconMessage } from "@/components/message";
import { useDelayedLoading } from "@/lib/hooks/use_delayed_loading";
import { useReplayBrowserList, useReplayBrowserNavigation } from "@/lib/hooks/use_replay_browser_list";
import { useReplayFilter } from "@/lib/hooks/use_replay_filter";
import { useReplayPresenter, useReplays } from "@/lib/hooks/use_replays";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

import { FileList } from "./file_list/file_list";
import { FileListSkeleton } from "./file_list/file_list_skeleton";
import { FilterToolbar } from "./filter_toolbar/filter_toolbar";
import { useFollowReplayList, usePlayFromReplayList } from "./play_from_list";
import { ReplayBrowserMessages as Messages } from "./replay_browser.messages";

export const ReplayBrowser = React.memo(() => {
  const presenter = useReplayPresenter();
  const searchInputRef = React.createRef<HTMLInputElement>();
  const { replayService } = useServices();

  const loading = useReplays((store) => store.loading);
  const replayProgress = useReplays((store) => store.progress);
  const loadingMore = useReplays((store) => store.loadingMore);
  const hasMoreReplays = useReplays((store) => store.hasMoreReplays);
  const showReplayProgress = React.useMemo(
    () => replayProgress && replayProgress.current !== replayProgress.total,
    [replayProgress],
  );
  const replayProgressPercent = React.useMemo(() => {
    if (replayProgress) {
      return Math.round((replayProgress.current / replayProgress.total) * 100);
    } else {
      return undefined;
    }
  }, [replayProgress]);

  const currentFolder = useReplays((store) => store.currentFolder);
  const folderTree = useReplays((store) => store.folderTree);

  const showLoading = useDelayedLoading(loading, 300, 500);
  const { showError, showSuccess } = useToasts();

  const resetFilter = useReplayFilter((store) => store.resetFilter);
  const { files: filteredFiles } = useReplayBrowserList();
  const { goToReplayStatsPage } = useReplayBrowserNavigation();

  const setSelectedItem = (index: number | undefined) => {
    if (index == null) {
      void presenter.clearSelectedFile();
    } else {
      const file = filteredFiles[index];
      void presenter.selectFile(file, index, filteredFiles.length);
      goToReplayStatsPage(file.fullPath);
    }
  };

  const onFolderTreeNodeClick = (fullPath: string) => {
    presenter.loadFolder(fullPath).catch(showError);
  };

  // With the list as the playback Dolphin's << >> (play_from_list.ts).
  const playFromList = usePlayFromReplayList();
  useFollowReplayList();
  const playSelectedFile = (index: number) => {
    void playFromList(filteredFiles[index].fullPath);
  };

  const handleLoadMore = React.useCallback(() => {
    if (!loadingMore && hasMoreReplays) {
      presenter.loadMoreReplays().catch(showError);
    }
  }, [presenter, loadingMore, hasMoreReplays, showError]);

  const deleteReplays = React.useCallback(
    (fileIds: string[]) => {
      replayService
        .deleteReplays(fileIds)
        .then(() => {
          // Don't optimistically remove the files in the UI since it could trigger an
          // infinite scroll and fetch more data, _before_ we can delete the files from the DB.
          presenter.removeFilesByIds(fileIds);
          showSuccess(Messages.filesDeleted(fileIds.length));
        })
        .catch(showError);
    },
    [presenter, showError, showSuccess, replayService],
  );

  // The rows' handlers stay the same functions across renders (they call the latest of these): a
  // new one would draw every loaded row again (React.memo) whenever this page renders, which with
  // a thousand replays loaded blocked the launcher for a second at each page the list loaded as it
  // was scrolled (bug report #268).
  const latest = React.useRef({ setSelectedItem, playSelectedFile, filteredFiles, deleteReplays });
  latest.current = { setSelectedItem, playSelectedFile, filteredFiles, deleteReplays };
  const onSelectRow = React.useCallback((index: number) => latest.current.setSelectedItem(index), []);
  const onPlayRow = React.useCallback((index: number) => latest.current.playSelectedFile(index), []);
  const onDeleteRow = React.useCallback((filePath: string) => {
    const file = latest.current.filteredFiles.find((f) => f.fullPath === filePath);
    if (file) {
      latest.current.deleteReplays([file.id]);
    }
  }, []);

  return (
    <Outer>
      <div
        style={{
          display: "flex",
          flex: "1",
          position: "relative",
          overflow: "hidden",
        }}
      >
        {/* One column (docs/design/launcher-redesign.md §7): the folders are a dropdown before the filters. */}
        <div
          css={css`
            display: flex;
            flex-direction: column;
            flex: 1;
            min-width: 0;
          `}
        >
          <div
            css={css`
              display: flex;
              align-items: center;
              gap: 8px;
              min-width: 0;
              padding-left: 28px;
            `}
          >
            <FolderSelect folders={folderTree} value={currentFolder} onChange={(path) => onFolderTreeNodeClick(path)} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <FilterToolbar ref={searchInputRef} />
            </div>
            {/* While the folder is read: how far along (the bar at the foot that also said it is gone). */}
            {showLoading && showReplayProgress && (
              <span
                style={{ display: "inline-flex", alignItems: "center", gap: 8, fontSize: 12, whiteSpace: "nowrap" }}
              >
                {Messages.processedReplayCount(replayProgress!.current, replayProgress!.total)}
                <CircularProgress color="secondary" size={16} value={replayProgressPercent} variant="determinate" />
              </span>
            )}
          </div>
          {showLoading ? (
            <FileListSkeleton />
          ) : filteredFiles.length === 0 ? (
            <EmptyFolder
              onClearFilter={() => {
                if (searchInputRef.current) {
                  searchInputRef.current.value = "";
                }
                resetFilter();
              }}
            />
          ) : (
            <FileList
              folderPath={currentFolder}
              onDelete={onDeleteRow}
              onSelect={onSelectRow}
              onPlay={onPlayRow}
              files={filteredFiles}
              onLoadMore={handleLoadMore}
              loadingMore={loadingMore}
              hasMoreReplays={hasMoreReplays}
            />
          )}
        </div>
      </div>
    </Outer>
  );
});

const EmptyFolder = ({ onClearFilter }: { onClearFilter: () => void }) => {
  return (
    <IconMessage Icon={SearchIcon} label={Messages.noSlpFilesFound()}>
      <div style={{ textAlign: "center" }}>
        <Button
          css={css`
            text-transform: lowercase;
            font-size: 12px;
          `}
          color="primary"
          onClick={onClearFilter}
          size="small"
        >
          {Messages.clearFilter()}
        </Button>
      </div>
    </IconMessage>
  );
};

// (min-height: 0: in the tabs' column it would grow to every row's height, scrolling the whole page
// with the folders and the footer, and the list's own scroll box never scrolling.)
const Outer = styled.div`
  display: flex;
  flex-flow: column;
  flex: 1;
  position: relative;
  min-width: 0;
  min-height: 0;
`;

/** The replay folders as a dropdown (each a level deeper indented), in place of the folder tree's column. */
const FolderSelect = ({
  folders,
  value,
  onChange,
}: {
  folders: readonly FolderResult[];
  value: string;
  onChange: (path: string) => void;
}) => {
  const flat: Array<{ folder: FolderResult; depth: number }> = [];
  const walk = (list: readonly FolderResult[], depth: number) => {
    for (const f of list) {
      flat.push({ folder: f, depth });
      walk(f.subdirectories, depth + 1);
    }
  };
  walk(folders, 0);
  if (flat.length === 0) {
    return null;
  }
  return (
    <Select
      size="small"
      value={flat.some((f) => f.folder.fullPath === value) ? value : ""}
      onChange={(e) => onChange(e.target.value)}
      renderValue={(v) => (
        <span style={{ display: "inline-flex", alignItems: "center", gap: 6, minWidth: 0, lineHeight: 1 }}>
          <FolderIcon fontSize="small" style={{ color: "var(--text-dim)", flex: "none" }} />
          {flat.find((f) => f.folder.fullPath === v)?.folder.name ?? ""}
        </span>
      )}
      sx={{
        minWidth: 150,
        maxWidth: 220,
        height: 34,
        flexShrink: 0,
        // The folder icon and the name on one centred line.
        "& .MuiSelect-select": {
          display: "flex",
          alignItems: "center",
          paddingTop: 0,
          paddingBottom: 0,
          height: "100%",
        },
      }}
      inputProps={{ "data-testid": "replay-folder" }}
    >
      {flat.map(({ folder, depth }) => (
        <MenuItem key={folder.fullPath} value={folder.fullPath} sx={{ paddingLeft: `${16 + depth * 16}px` }}>
          {folder.name}
        </MenuItem>
      ))}
    </Select>
  );
};
