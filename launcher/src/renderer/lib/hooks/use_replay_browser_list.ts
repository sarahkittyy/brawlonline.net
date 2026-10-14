import { useNavigate } from "react-router-dom";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { useReplayPresenter, useReplays } from "@/lib/hooks/use_replays";

const useReplayBrowserNavigationStore = create(
  combine(
    {
      lastPath: "/main/replays/list",
    },
    (set) => ({
      setLastPath: (lastPath: string) => set({ lastPath }),
    }),
  ),
);

export const useReplayBrowserNavigation = () => {
  const navigate = useNavigate();
  const lastPath = useReplayBrowserNavigationStore((store) => store.lastPath);
  const setLastPath = useReplayBrowserNavigationStore((store) => store.setLastPath);

  const goToPage = (pageUrl: string) => {
    setLastPath(pageUrl);
    navigate(pageUrl);
  };

  const goToReplayStatsPage = (filePath: string) => {
    const encodedPath = encodeURIComponent(filePath);
    const pageUrl = `/main/replays/${encodedPath}`;
    goToPage(pageUrl);
  };

  const goToReplayList = () => {
    const pageUrl = `/main/replays/list`;
    goToPage(pageUrl);
  };

  return {
    lastPath,
    goToReplayStatsPage,
    goToReplayList,
  };
};

export const useReplayBrowserList = () => {
  const presenter = useReplayPresenter();
  const files = useReplays((store) => store.files);
  const selectedFile = useReplays((store) => store.selectedFile);
  // The open file's place in the list now: a refresh (opening Replays) can list new games above it
  const openPath = selectedFile.fileResult?.fullPath;
  const found = openPath ? files.findIndex((f) => f.fullPath === openPath) : -1;
  const index = found >= 0 ? found : selectedFile.index;
  const total = found >= 0 ? files.length : selectedFile.total;
  const { goToReplayStatsPage } = useReplayBrowserNavigation();

  const setSelectedItem = (index: number | undefined) => {
    if (index == null) {
      presenter.clearSelectedFile();
      return;
    }
    const file = files[index];
    presenter.selectFile(file, index, files.length);
    goToReplayStatsPage(file.fullPath);
  };

  const selectNextFile = () => {
    if (index == null) {
      return;
    }
    if (index < files.length - 1) {
      setSelectedItem(index + 1);
    }
  };

  const selectPrevFile = () => {
    if (index == null) {
      return;
    }
    if (index > 0) {
      setSelectedItem(index - 1);
    }
  };

  return {
    files,
    index,
    total,
    selectNextFile,
    selectPrevFile,
  };
};
