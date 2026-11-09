import Button from "@mui/material/Button";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import type { GameEnding, MatchReplay } from "@replays/types";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";

import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { UserName } from "@/components/user_name/user_name";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import { useReplayFilter } from "@/lib/hooks/use_replay_filter";
import { useReplayHighlight } from "@/lib/hooks/use_replay_highlight";
import { useReplayPresenter } from "@/lib/hooks/use_replays";
import { useSettings } from "@/lib/hooks/use_settings";
import {
  formatMatchScore,
  formatPlace,
  formatRating,
  formatRatingChange,
  HIDDEN_RATING,
  matchNote,
  matchSides,
  modeLabel,
  playerName,
  ratingChangeColor,
  TEAM_COLORS,
  teamName,
} from "@/lib/ranked_format";
import { monthDayHourFormat } from "@/lib/time";
import { useServices } from "@/services";
import type { AuthUser } from "@/services/auth/types";
import type { MatchHistoryFilter, MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import { MatchHistoryPageMessages as Messages } from "./match_history_page.messages";

/**
 * The logged-in player's matches (docs/design/replays.md §9.2 for the replays column). Their own name,
 * rank and rating are on their profile (the user menu's View Profile, docs/design/profiles.md).
 */
export const MatchHistoryPage = React.memo(function MatchHistoryPage({ user }: { user: AuthUser }) {
  return (
    <div style={{ display: "flex", flexDirection: "column", flex: 1, minWidth: 0, padding: "20px 20px 0" }}>
      <MatchHistory uid={user.uid} />
    </div>
  );
});

const MatchHistory = ({ uid }: { uid: string }) => {
  const { backendService } = useServices();
  const [mode, setMode] = useState<MatchHistoryFilter>("all");
  const fetchPage = useCallback(
    async (before: string | undefined) => {
      const page = await backendService.fetchMatchHistory(mode, before);
      return { items: page.matches, next: page.next };
    },
    [backendService, mode],
  );
  // Fresh on every visit and when the window regains focus (a set settles while the player is in
  // Dolphin), like the rating above; else a set shows "In progress" until the launcher restarts.
  const pages = useCursorPages<MatchHistoryItem>({ queryKey: ["matchHistory", uid, mode], fetchPage, refresh: true });
  const replays = useMatchReplays(pages.items);

  return (
    <>
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", marginBottom: 8 }}>
        <Typography variant="h6">{Messages.matchHistory()}</Typography>
        <ToggleButtonGroup
          size="small"
          exclusive={true}
          value={mode}
          onChange={(_, value: MatchHistoryFilter | null) => value && setMode(value)}
        >
          <ToggleButton value="all">{Messages.all()}</ToggleButton>
          <ToggleButton value="ranked">{Messages.ranked()}</ToggleButton>
          <ToggleButton value="unranked">{Messages.unranked()}</ToggleButton>
          <ToggleButton value="rooms">{Messages.rooms()}</ToggleButton>
          <ToggleButton value="twos">{Messages.twos()}</ToggleButton>
        </ToggleButtonGroup>
      </div>
      <div style={{ flex: 1, minHeight: 0 }}>
        <InfiniteScrollContainer
          onLoadMore={pages.loadMore}
          hasMore={pages.hasMore && !pages.error}
          isLoading={pages.isLoading || pages.isLoadingMore}
          resetKey={mode}
        >
          {pages.items.length > 0 && (
            <Table size="small" stickyHeader={true}>
              <TableHead>
                <TableRow>
                  <TableCell>{Messages.mode()}</TableCell>
                  <TableCell>{Messages.date()}</TableCell>
                  <TableCell>{Messages.you()}</TableCell>
                  <TableCell align="center">{Messages.score()}</TableCell>
                  <TableCell>{Messages.opponent()}</TableCell>
                  <TableCell>{Messages.replays()}</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {pages.items.map((m) => (
                  <MatchRow key={m.matchId} item={m} myUid={uid} replays={replays.data?.[m.matchId] ?? null} />
                ))}
              </TableBody>
            </Table>
          )}
          {!pages.isLoading && !pages.error && pages.items.length === 0 && (
            <Typography variant="body2" color="text.secondary" sx={{ padding: 2 }}>
              {Messages.noMatches()}
            </Typography>
          )}
          <PagedListStatus pages={pages} />
        </InfiniteScrollContainer>
      </div>
    </>
  );
};

/**
 * The replays on this computer of each listed match (docs/design/replays.md §9.2): every player's
 * Dolphin saves the games it played, named by match in the file, so the server's match id finds
 * them in the replay folders. Looked up again when the list grows or the folders change.
 */
const useMatchReplays = (items: MatchHistoryItem[]) => {
  const { replayService } = useServices();
  const rootSlpPath = useSettings((store) => store.settings.rootSlpPath);
  const extraSlpPaths = useSettings((store) => store.settings.extraSlpPaths);
  const matchIds = useMemo(() => items.map((m) => m.matchId), [items]);
  return useQuery({
    queryKey: ["matchReplays", matchIds, rootSlpPath, extraSlpPaths],
    queryFn: () => replayService.findReplaysForMatches(matchIds, [rootSlpPath, ...extraSlpPaths]),
    enabled: matchIds.length > 0,
    refetchOnWindowFocus: "always",
  });
};

const dirname = (p: string) => p.slice(0, Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\")));

/** The replay folder (Settings > Replays: the root or an extra one) that holds `file`, else its own folder. */
const replayRootOf = (file: string, roots: string[]): string => {
  const holding = roots.filter((r) => r && window.electron.utils.isSubdirectory(r, file));
  return holding.sort((a, b) => b.length - a.length)[0] ?? dirname(file);
};

/** A match's replays: the button opens Replays, refreshed, with them pulsing gold (a set's games are one file each). */
const ReplaysCell = ({ replays }: { replays: MatchReplay[] | null }) => {
  if (!replays || replays.length === 0) {
    return (
      <Tooltip title={Messages.noReplaysHint()}>
        <span style={{ fontSize: 12, color: "var(--text-secondary)" }}>{Messages.noReplays()}</span>
      </Tooltip>
    );
  }
  return <ShowReplaysButton replays={replays.map((r) => r.path)} />;
};

/** Under the score: the games on this computer that added no win, so 0 - 2 beside 3 replays adds up. */
const NoWinGamesNote = ({ replays }: { replays: MatchReplay[] | null }) => {
  const count = (ending: GameEnding) => replays?.filter((r) => r.ending === ending).length ?? 0;
  const text = Messages.noWinGames({
    draw: count("draw"),
    quit: count("quit"),
    desync: count("desync"),
    unfinished: count("unfinished"),
  });
  if (!text) {
    return null;
  }
  return (
    <Tooltip title={Messages.noWinGamesHint()}>
      <div style={{ fontSize: 12, fontWeight: "normal", whiteSpace: "normal", color: "var(--text-secondary)" }}>
        {text}
      </div>
    </Tooltip>
  );
};

const ShowReplaysButton = ({ replays }: { replays: string[] }) => {
  const presenter = useReplayPresenter();
  const navigate = useNavigate();
  const setSearchText = useReplayFilter((store) => store.setSearchText);
  const highlight = useReplayHighlight((store) => store.highlight);
  const rootSlpPath = useSettings((store) => store.settings.rootSlpPath);
  const extraSlpPaths = useSettings((store) => store.settings.extraSlpPaths);
  const show = () => {
    // All replays (the replay folder with its month folders), newest first, no search hiding them
    setSearchText("");
    highlight(replays);
    void presenter.loadFolder(replayRootOf(replays[0], [rootSlpPath, ...extraSlpPaths]), true).catch(() => undefined);
    navigate("/main/replays/list");
  };
  return (
    <Tooltip title={Messages.showHint(replays.length)}>
      <Button size="small" variant="outlined" onClick={show} style={{ whiteSpace: "nowrap" }}>
        {Messages.show(replays.length)}
      </Button>
    </Tooltip>
  );
};

export const MatchRow = React.memo(function MatchRow({
  item,
  myUid,
  replays = null,
}: {
  item: MatchHistoryItem;
  myUid: string;
  replays?: MatchReplay[] | null;
}) {
  // A room's team battle: my teammates on my side. A free-for-all: everyone else's place.
  const [mine, others] = matchSides(item, myUid);
  const showPlaces = item.room?.mode === "ffa";
  const note = matchNote(item);
  return (
    <TableRow>
      <TableCell>
        <div>{modeLabel(item.mode, item.room?.mode)}</div>
        {note && <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>{note}</div>}
        {item.room?.ruleset && <RulesetLink ruleset={item.room.ruleset} />}
      </TableCell>
      <TableCell sx={{ whiteSpace: "nowrap" }}>{monthDayHourFormat(new Date(item.createdAt))}</TableCell>
      <TableCell>
        {mine.map((p) => (
          <PlayerCell key={p.uid} player={p} />
        ))}
      </TableCell>
      <TableCell align="center" sx={{ fontWeight: "bold", whiteSpace: "nowrap" }}>
        {formatMatchScore(item, myUid)}
        <NoWinGamesNote replays={replays} />
      </TableCell>
      <TableCell>
        {others.map((p) => (
          <PlayerCell key={p.uid} player={p} showPlace={showPlaces} showWins={item.mode === "twos"} />
        ))}
      </TableCell>
      <TableCell>
        <ReplaysCell replays={replays} />
      </TableCell>
    </TableRow>
  );
});

/** A room game's published ruleset: its name, opening its page on the website (`/r/<code>`). */
const RulesetLink = ({ ruleset }: { ruleset: { code: string; name: string; version: number; link: string } }) => (
  <a
    href={ruleset.link}
    title={Messages.rulesetPage(ruleset.code)}
    onClick={(e) => {
      e.preventDefault();
      window.electron.shell.openExternal(ruleset.link).catch(() => undefined);
    }}
    style={{ fontSize: 12, color: "var(--text-secondary)" }}
  >
    {ruleset.version > 0 ? `${ruleset.name} v${ruleset.version}` : ruleset.name}
  </a>
);

const PlayerCell = ({
  player,
  showPlace = false,
  showWins = false,
}: {
  player: MatchHistoryPlayer;
  showPlace?: boolean;
  /** A 2v2 match: the games the player's team won (the teams change every game). */
  showWins?: boolean;
}) => {
  const team = player.team;
  return (
    <div>
      {team != null && TEAM_COLORS[team] && (
        <span
          title={teamName(team)}
          aria-label={teamName(team)}
          style={{
            display: "inline-block",
            width: 8,
            height: 8,
            borderRadius: "50%",
            marginRight: 6,
            verticalAlign: "middle",
            backgroundColor: TEAM_COLORS[team],
          }}
        />
      )}
      <UserName
        user={{ ...player, displayName: playerName(player) }}
        showBadge={true}
        profileLink={player.displayName !== ""}
      />
      {showPlace && player.place != null && (
        <span style={{ marginLeft: 8, fontWeight: "bold" }}>{formatPlace(player.place)}</span>
      )}
      {showWins && (
        <span style={{ marginLeft: 8, color: "var(--text-secondary)" }}>{Messages.twosWins(player.wins)}</span>
      )}
      {player.ratingHidden && (
        <div style={{ fontSize: 12, color: "var(--text-secondary)" }} title={Messages.ratingHidden()}>
          {HIDDEN_RATING}
        </div>
      )}
      {player.ratingBefore != null && player.ratingChange != null && (
        <div style={{ fontSize: 12 }}>
          <span style={{ color: "var(--text-secondary)" }}>{formatRating(player.ratingBefore)}</span>{" "}
          <span style={{ color: ratingChangeColor(player.ratingChange) }}>
            {formatRatingChange(player.ratingChange)}
          </span>
        </div>
      )}
    </div>
  );
};
