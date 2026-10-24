import { css } from "@emotion/react";
import styled from "@emotion/styled";
import FolderIcon from "@mui/icons-material/Folder";
import SearchIcon from "@mui/icons-material/Search";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import List from "@mui/material/List";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { DualPane } from "@/components/dual_pane";
import { BasicFooter } from "@/components/footer/footer";
import { LabelledText } from "@/components/labelled_text";
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
import { FolderTreeNode } from "./folder_tree_node";
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
  const collapsedFolders = useReplays((store) => store.collapsedFolders);
  const totalFilesInFolder = useReplays((store) => store.totalFilesInFolder);

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
        <DualPane
          id="replay-browser"
          resizable={true}
          minWidth={0}
          maxWidth={300}
          leftStyle={{ backgroundColor: "rgba(0,0,0, 0.3)" }}
          leftSide={
            <List dense={true} style={{ flex: 1, padding: 0 }}>
              <div style={{ position: "relative", minHeight: "100%" }}>
                {folderTree.map((folder) => {
                  return (
                    <FolderTreeNode
                      folder={folder}
                      key={folder.fullPath}
                      collapsedFolders={collapsedFolders}
                      onClick={onFolderTreeNodeClick}
                      onToggle={(folder) => presenter.toggleFolder(folder)}
                    />
                  );
                })}
              </div>
            </List>
          }
          rightSide={
            <div
              css={css`
                display: flex;
                flex-direction: column;
                flex: 1;
              `}
            >
              <FilterToolbar ref={searchInputRef} />
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
                  onDelete={(filePath) => {
                    const file = filteredFiles.find((f) => f.fullPath === filePath);
                    if (file) {
                      deleteReplays([file.id]);
                    }
                  }}
                  onSelect={(index: number) => setSelectedItem(index)}
                  onPlay={(index: number) => playSelectedFile(index)}
                  files={filteredFiles}
                  onLoadMore={handleLoadMore}
                  loadingMore={loadingMore}
                  hasMoreReplays={hasMoreReplays}
                />
              )}
            </div>
          }
        />
      </div>

      <Footer>
        <div
          css={css`
            display: flex;
            align-items: center;
          `}
        >
          <div>
            <Tooltip title={Messages.revealLocation()}>
              <IconButton onClick={() => window.electron.shell.openPath(currentFolder)} size="small">
                <FolderIcon
                  css={css`
                    color: var(--surface-3);
                  `}
                />
              </IconButton>
            </Tooltip>
          </div>
          <LabelledText
            label={Messages.currentFolder()}
            css={css`
              margin-left: 10px;
            `}
          >
            {currentFolder}
          </LabelledText>
        </div>
        <div
          css={css`
            display: flex;
            align-items: center;
            justify-content: flex-end;
            gap: 1em;
          `}
        >
          {showLoading ? (
            <>
              <span>
                {showReplayProgress
                  ? Messages.processedReplayCount(replayProgress!.current, replayProgress!.total)
                  : ""}
              </span>
              <CircularProgress
                color="secondary"
                size={20}
                value={replayProgressPercent}
                variant={replayProgressPercent ? "determinate" : "indeterminate"}
              />
            </>
          ) : (
            Messages.totalReplayCount(totalFilesInFolder ?? filteredFiles.length)
          )}
        </div>
      </Footer>
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

const Outer = styled.div`
  display: flex;
  flex-flow: column;
  flex: 1;
  position: relative;
  min-width: 0;
`;

const Footer = styled(BasicFooter)`
  justify-content: space-between;
`;
