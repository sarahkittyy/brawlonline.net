import { exists } from "@common/exists";
import { pplusReleaseLabel } from "@common/pplus_releases";
import { stageInfo as pplusStageInfo } from "@common/stages";
import type { RenderJobInfo } from "@dolphin/types";
import { css } from "@emotion/react";
import EventIcon from "@mui/icons-material/Event";
import HistoryIcon from "@mui/icons-material/History";
import LandscapeIcon from "@mui/icons-material/Landscape";
import MoreHorizIcon from "@mui/icons-material/MoreHoriz";
import MusicNoteIcon from "@mui/icons-material/MusicNote";
import MusicOffIcon from "@mui/icons-material/MusicOff";
import PlayCircleOutlineIcon from "@mui/icons-material/PlayCircleOutline";
import SportsCricket from "@mui/icons-material/SportsCricket";
import TimerIcon from "@mui/icons-material/Timer";
import TrackChangesIcon from "@mui/icons-material/TrackChanges";
import type { FileResult } from "@replays/types";
import { frameToGameTimer, GameMode } from "@slippi/slippi-js";
import { groupBy } from "common/group_by";
import React, { useCallback, useMemo, useState } from "react";

import { ClapperboardIcon } from "@/components/clapperboard_icon";
import { IconMenu } from "@/components/icon_menu";
import { RenderVideoIcon } from "@/components/render_video_icon";
import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useReplayHighlight } from "@/lib/hooks/use_replay_highlight";
import { useReplays } from "@/lib/hooks/use_replays";
import { usePPlusVersions } from "@/lib/pplus/use_pplus_versions";
import { convertFrameCountToDurationString, monthDayHourFormat } from "@/lib/time";
import { getStageImage } from "@/lib/utils";
import { REPLAY_LIST, usePlaylistStore } from "@/pages/replays/replay_file_stats/highlights/highlight_nav";
import { useServices } from "@/services";

import { DraggableFile } from "./draggable_file";
import type { ReplayDetail, ReplayFileAction } from "./replay_file";
import { ReplayFile as ReplayFileImpl } from "./replay_file";
import { ReplayFileMessages as Messages } from "./replay_file.messages";
import type { PlayerInfo } from "./team_elements/team_elements";
import { useFileDrag } from "./use_file_drag";

type ReplayFileContainerProps = FileResult & {
  index: number;
  onSelect: (index: number) => void;
  onPlay: (index: number) => void;
  onOpenMenu: (index: number, element: HTMLElement) => void;
  onClick: (index: number, isShiftHeld: boolean) => void;
  selectedFilesSet: Set<string>;
  /** This replay's whole-match render still going (Render to video), if any. */
  matchRender?: RenderJobInfo;
};

export const ReplayFileContainer = React.memo(function ReplayFileContainer({
  index,
  onOpenMenu,
  onSelect,
  onPlay,
  onClick,
  id,
  fileName,
  game,
  fullPath,
  selectedFilesSet,
  matchRender,
}: ReplayFileContainerProps) {
  // Our replays name P+'s stage ids (the stage select's, @common/stages), not Melee's.
  const stageInfo = game.stageId != null ? pplusStageInfo(game.stageId) : undefined;
  const stageImageUrl = stageInfo != null ? getStageImage(stageInfo.id) : undefined;
  const stageName = stageInfo != null ? stageInfo.name : Messages.unknownStage();

  // Use Set for O(1) lookup instead of O(n) indexOf
  const selected = selectedFilesSet.has(fullPath);
  const highlightKey = useReplayHighlight((store) => (store.paths.includes(fullPath) ? store.nonce : 0));
  // The replay the playback Dolphin plays from this list (its << >> follow).
  const playing = usePlaylistStore((store) => store.source === REPLAY_LIST && store.current?.path === fullPath);

  // Only calculate index if actually selected (for display purposes)
  const selectedFiles = useReplays((store) => store.selectedFiles);
  const selectedIndex = useMemo(
    () => (selected ? selectedFiles.indexOf(fullPath) : -1),
    [selected, selectedFiles, fullPath],
  );

  // Pre-compute drag paths to avoid recreating array
  const dragPaths = useMemo(
    () => (selected && selectedFiles.length > 0 ? selectedFiles : []),
    [selected, selectedFiles],
  );

  const fileDrag = useFileDrag();
  const onShowStats = useCallback(() => onSelect(index), [onSelect, index]);
  const onReplayClick = useCallback(
    (e: React.MouseEvent<HTMLDivElement, MouseEvent>) => onClick(index, e.shiftKey),
    [onClick, index],
  );
  const onPlayClick = useCallback(() => onPlay(index), [onPlay, index]);

  // Render to video: the whole match, as highlights are rendered (Settings > Clips' resolution, into
  // its folder), named as the replay is. The button opens Render (w/ Music) / Render (No Music);
  // while it renders it shows how far it is and cancels it.
  const { dolphinService } = useServices();
  const { renderHighlights } = useDolphinActions(dolphinService);
  const [renderMenu, setRenderMenu] = useState<HTMLElement | null>(null);
  const matchRenderId = matchRender?.jobId;
  const onRenderClick = useCallback(
    (e: React.MouseEvent<HTMLButtonElement, MouseEvent>) => {
      if (matchRenderId) {
        void dolphinService.cancelRender(matchRenderId);
      } else {
        setRenderMenu(e.currentTarget);
      }
    },
    [matchRenderId, dolphinService],
  );
  const renderMatch = useCallback(
    (music: boolean) => {
      setRenderMenu(null);
      void renderHighlights({
        items: [{ path: fullPath, startFrame: 0, endFrame: 0 }],
        names: [fileName.replace(/\.brep$/i, "")],
        music,
        combo: false,
        wholeMatch: true,
      });
    },
    [renderHighlights, fullPath, fileName],
  );
  const renderState = matchRender?.state;
  const renderProgress = matchRender?.progress ?? 0;

  // Drag handler for the entire replay item (drags all selected files)
  const onReplayDragStart = useMemo(() => {
    if (dragPaths.length === 0) {
      return undefined;
    }
    return (event: React.DragEvent<HTMLDivElement>) => {
      fileDrag(event, dragPaths);
    };
  }, [fileDrag, dragPaths]);

  // Drag handler for just the filename (drags only this file)
  const onFileNameDragStart = useCallback(
    (event: React.DragEvent<HTMLDivElement>) => {
      fileDrag(event, [fullPath]);
    },
    [fileDrag, fullPath],
  );

  const actions = useMemo((): ReplayFileAction[] => {
    return [
      {
        Icon: MoreHorizIcon,
        label: Messages.moreOptions(),
        onClick: (e: React.MouseEvent<HTMLButtonElement, MouseEvent>) => onOpenMenu(index, e.currentTarget),
      },
      {
        Icon: RenderVideoIcon,
        label:
          renderState === "queued"
            ? Messages.renderWaiting()
            : renderState
            ? Messages.renderingToVideo(Math.round(renderProgress * 100))
            : Messages.renderToVideo(),
        onClick: onRenderClick,
        progress: renderState === "rendering" ? renderProgress : renderState ? null : undefined,
        isDolphinAction: !renderState,
      },
      {
        Icon: ClapperboardIcon,
        label: Messages.showHighlights(),
        onClick: onShowStats,
      },
      {
        Icon: PlayCircleOutlineIcon,
        label: Messages.launchReplay(),
        primary: true,
        onClick: onPlayClick,
        disabled: false,
        isDolphinAction: true,
      },
    ];
  }, [index, onOpenMenu, onPlayClick, onShowStats, onRenderClick, renderState, renderProgress]);

  // Recorded on another Project+ release than the launcher's: said on the row (it plays on that
  // release's files, docs/design/pplus-versions.md).
  const currentRelease = usePPlusVersions((s) => s.current);
  const olderRelease =
    game.pplusRelease && currentRelease && game.pplusRelease !== currentRelease ? game.pplusRelease : undefined;

  const details = useMemo(() => {
    const date = new Date(game.startTime ? Date.parse(game.startTime) : 0);
    const list = generateReplayDetails({
      gameMode: game.mode,
      date,
      lastFrame: game.lastFrame,
      timerType: game.timerType,
      startingTimerSeconds: game.startingTimerSeconds,
      stageName,
    });
    if (olderRelease) {
      list.push({ Icon: HistoryIcon, label: Messages.pplusRelease(pplusReleaseLabel(olderRelease)) });
    }
    return list;
  }, [game.lastFrame, game.mode, game.startTime, game.startingTimerSeconds, game.timerType, stageName, olderRelease]);

  const players = useMemo((): PlayerInfo[][] => {
    const teams = Object.values(groupBy(game.players, (p) => (game.isTeams ? p.teamId : p.port)));
    return teams.map((team) => {
      return team.map((player): PlayerInfo => {
        const backupName = player.type === 1 ? "CPU" : `Player ${player.playerIndex + 1}`;
        const teamId = game.isTeams ? player.teamId : undefined;
        return {
          characterId: player.characterId,
          characterColor: player.characterColor,
          port: player.port,
          teamId,
          variant: player.connectCode ? "code" : "tag",
          text: player.connectCode || player.tag || backupName,
          isWinner: player.isWinner,
        };
      });
    });
  }, [game.isTeams, game.players]);

  return (
    <DraggableFile onDragStart={onReplayDragStart}>
      <div
        key={id}
        css={css`
          cursor: pointer;
        `}
        onClick={onReplayClick}
        // A double-click opens the stats page, as the stats button does (its two clicks select
        // and deselect the row, leaving the selection as it was).
        onDoubleClick={onShowStats}
      >
        <ReplayFileImpl
          fileName={fileName}
          backgroundImage={stageImageUrl}
          selectedIndex={selected ? selectedIndex : undefined}
          players={players}
          actions={actions}
          details={details}
          onFileNameDragStart={onFileNameDragStart}
          highlightKey={highlightKey}
          playing={playing}
        />
      </div>
      {/* (Its clicks are kept from the row, which would select it: the menu is in a portal.) */}
      <IconMenu
        anchorEl={renderMenu}
        open={renderMenu != null}
        onClose={() => setRenderMenu(null)}
        onClick={(e) => e.stopPropagation()}
        onDoubleClick={(e) => e.stopPropagation()}
        anchorOrigin={{ vertical: "bottom", horizontal: "center" }}
        transformOrigin={{ vertical: "top", horizontal: "center" }}
        items={[
          {
            label: Messages.renderWithMusic(),
            icon: <MusicNoteIcon fontSize="small" />,
            onClick: () => renderMatch(true),
          },
          {
            label: Messages.renderNoMusic(),
            icon: <MusicOffIcon fontSize="small" />,
            onClick: () => renderMatch(false),
          },
        ]}
      />
    </DraggableFile>
  );
});

const generateReplayDetails = ({
  date,
  stageName,
  gameMode,
  lastFrame,
  timerType,
  startingTimerSeconds,
}: {
  date: Date;
  stageName: string;
  gameMode?: number;
  lastFrame?: number;
  timerType?: number;
  startingTimerSeconds?: number;
}): ReplayDetail[] => {
  const replayDetails: ReplayDetail[] = [
    {
      Icon: EventIcon,
      label: monthDayHourFormat(new Date(date)) ?? "",
    },
  ];

  if (exists(lastFrame) && gameMode !== GameMode.HOME_RUN_CONTEST) {
    replayDetails.push({
      Icon: TimerIcon,
      label:
        gameMode === GameMode.TARGET_TEST
          ? frameToGameTimer(lastFrame, { timerType, startingTimerSeconds })
          : convertFrameCountToDurationString(lastFrame, "long"),
    });
  }

  replayDetails.push({
    Icon: getReplayStageIcon(gameMode),
    label: stageName,
  });
  return replayDetails;
};

const getReplayStageIcon = (gameMode: number | undefined): React.ComponentType => {
  switch (gameMode) {
    case GameMode.HOME_RUN_CONTEST:
      return SportsCricket;
    case GameMode.TARGET_TEST:
      return TrackChangesIcon;
    default:
      break;
  }
  return LandscapeIcon;
};
