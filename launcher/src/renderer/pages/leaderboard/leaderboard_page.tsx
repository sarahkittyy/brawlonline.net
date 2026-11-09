import CloseIcon from "@mui/icons-material/Close";
import SearchIcon from "@mui/icons-material/Search";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import React, { useCallback, useState } from "react";

import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { useAccount } from "@/lib/hooks/use_account";
import type { CursorPage } from "@/lib/hooks/use_cursor_pages";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import { useSearchText } from "@/lib/hooks/use_search_text";
import { formatRating, formatRecord, RATING_DOWN_COLOR, RATING_UP_COLOR } from "@/lib/ranked_format";
import { useServices } from "@/services";
import type { LeaderboardEntry } from "@/services/backend/types";

import { LeaderboardPageMessages as Messages } from "./leaderboard_page.messages";

type LeaderboardPageData = CursorPage<LeaderboardEntry> & { total: number };

/** How long a search's loaded pages (the whole board's too) are shown again without a request. */
const SEARCH_STALE_MS = 60_000;

/**
 * The ranked leaderboard: everyone with a rated set, by Elo rating. Slippi has its leaderboards
 * on slippi.gg rather than in the launcher; this is a plain table in the launcher's style.
 * Pages load as the list scrolls near its end. The search box (right of the title) finds players
 * by name or connect code on the server, as the player types: it waits for a pause in the typing
 * and sends at most one search a second ({@link useSearchText}), a search typed again (or the
 * cleared box) within {@link SEARCH_STALE_MS} shows its pages without asking again, and the last
 * list stays (dimmed) until the new one arrives.
 */
export const LeaderboardPage = React.memo(function LeaderboardPage() {
  const { backendService } = useServices();
  const myUid = useAccount((s) => s.user?.uid);
  const [text, setText] = useState("");
  const search = useSearchText(text);

  const fetchPage = useCallback(
    async (after: string | undefined): Promise<LeaderboardPageData> => {
      const page = await backendService.fetchLeaderboard(after, search || undefined);
      return { items: page.entries, next: page.next, total: page.total };
    },
    [backendService, search],
  );
  const pages = useCursorPages<LeaderboardEntry>({
    queryKey: ["leaderboard", search],
    fetchPage,
    keepPrevious: true,
    staleTime: SEARCH_STALE_MS,
  });
  const total = (pages.firstPage as LeaderboardPageData | undefined)?.total;

  return (
    <div style={{ display: "flex", flexDirection: "column", flex: 1, minWidth: 0, padding: "20px 20px 0" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 12, marginBottom: 12 }}>
        <div style={{ display: "flex", alignItems: "baseline", gap: 12, flex: 1, minWidth: 0 }}>
          <Typography variant="h5">{Messages.leaderboard()}</Typography>
          {total != null && (
            <Typography variant="body2" color="text.secondary">
              {Messages.rankedPlayers(total)}
            </Typography>
          )}
        </div>
        <TextField
          size="small"
          hiddenLabel={true}
          value={text}
          placeholder={Messages.searchPlaceholder()}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Escape" && text !== "") {
              e.stopPropagation();
              setText("");
            }
          }}
          inputProps={{
            "aria-label": Messages.searchPlaceholder(),
            "data-testid": "leaderboard-search",
            maxLength: 64,
          }}
          InputProps={{
            startAdornment: (
              <InputAdornment position="start">
                <SearchIcon fontSize="small" />
              </InputAdornment>
            ),
            endAdornment: (
              <InputAdornment position="end" sx={{ width: 28, justifyContent: "center" }}>
                {pages.isPrevious ? (
                  <CircularProgress color="inherit" size={14} />
                ) : text !== "" ? (
                  <IconButton size="small" aria-label={Messages.clearSearch()} onClick={() => setText("")}>
                    <CloseIcon fontSize="inherit" />
                  </IconButton>
                ) : null}
              </InputAdornment>
            ),
          }}
          sx={{ width: 280, flexShrink: 0 }}
        />
      </div>
      <div style={{ flex: 1, minHeight: 0 }}>
        <InfiniteScrollContainer
          onLoadMore={pages.loadMore}
          hasMore={pages.hasMore && !pages.error}
          isLoading={pages.isLoading || pages.isLoadingMore}
          resetKey={search}
        >
          {pages.error && pages.items.length === 0 ? null : (
            <Table
              size="small"
              stickyHeader={true}
              style={{ opacity: pages.isPrevious ? 0.5 : 1, transition: "opacity 150ms" }}
            >
              <LeaderboardHead />
              <TableBody>
                {pages.items.map((e) => (
                  <LeaderboardRow key={e.uid} entry={e} isMe={e.uid === myUid} />
                ))}
              </TableBody>
            </Table>
          )}
          {!pages.isLoading && !pages.error && pages.items.length === 0 && (
            <Typography variant="body2" color="text.secondary" sx={{ padding: 2 }}>
              {search ? Messages.noMatches(search) : Messages.empty()}
            </Typography>
          )}
          <PagedListStatus pages={pages} />
        </InfiniteScrollContainer>
      </div>
    </div>
  );
});

export const LeaderboardHead = React.memo(function LeaderboardHead() {
  return (
    <TableHead>
      <TableRow>
        <TableCell sx={{ width: 84 }}>
          <span style={positionStyle}>{Messages.position()}</span>
        </TableCell>
        <TableCell align="center" sx={{ width: 96 }}>
          {Messages.rank()}
        </TableCell>
        <TableCell>{Messages.player()}</TableCell>
        <TableCell>{Messages.connectCode()}</TableCell>
        <TableCell align="right">{Messages.rating()}</TableCell>
        <TableCell align="right">{Messages.record()}</TableCell>
      </TableRow>
    </TableHead>
  );
});

export const LeaderboardRow = React.memo(function LeaderboardRow({
  entry,
  isMe,
}: {
  entry: LeaderboardEntry;
  isMe: boolean;
}) {
  const strong = isMe ? { fontWeight: "bold" } : undefined;
  return (
    <TableRow data-me={isMe || undefined} sx={isMe ? { backgroundColor: "rgba(255, 255, 255, 0.1)" } : undefined}>
      <TableCell sx={{ whiteSpace: "nowrap", ...strong }}>
        <span style={positionStyle}>{entry.position}</span>
        <PositionChange change={entry.positionChange} />
      </TableCell>
      <TableCell align="center" sx={{ paddingY: "4px" }}>
        <RankBadge rank={entry.rank} setsToPlace={entry.setsToPlace} size="small" />
      </TableCell>
      <TableCell sx={strong}>{entry.displayName}</TableCell>
      <TableCell sx={{ color: "text.secondary", ...strong }}>{entry.connectCode}</TableCell>
      <TableCell align="right" sx={strong}>
        {formatRating(entry.rating)}
      </TableCell>
      <TableCell align="right" sx={strong}>
        {formatRecord(entry.wins, entry.losses)}
      </TableCell>
    </TableRow>
  );
});

/** The position, right-aligned in a fixed width so the change fits beside it. */
const positionStyle: React.CSSProperties = { display: "inline-block", width: 34, textAlign: "right" };

const changeStyle: React.CSSProperties = {
  display: "inline-flex",
  alignItems: "center",
  gap: 3,
  marginLeft: 8,
  fontSize: 11,
  fontWeight: 600,
};

/**
 * How far a player moved in the last 24 hours, small beside the position: a green triangle up and
 * the places, a red one down and `-places`, `NEW` for a player who placed since; nothing when
 * the player did not move (or from an older server).
 */
export const PositionChange = React.memo(function PositionChange({ change }: { change?: number | null }) {
  if (change === null) {
    return (
      <Tooltip title={Messages.newEntryTooltip()}>
        <span
          data-testid="position-change"
          style={{ ...changeStyle, color: RATING_UP_COLOR, fontSize: 9, letterSpacing: 0.5 }}
        >
          {Messages.newEntry().toUpperCase()}
        </span>
      </Tooltip>
    );
  }
  if (change === undefined || change === 0) {
    return null;
  }
  const up = change > 0;
  return (
    <Tooltip title={up ? Messages.movedUp(change) : Messages.movedDown(-change)}>
      <span data-testid="position-change" style={{ ...changeStyle, color: up ? RATING_UP_COLOR : RATING_DOWN_COLOR }}>
        <svg width={7} height={6} viewBox="0 0 7 6" aria-hidden={true}>
          <path d={up ? "M3.5 0L7 6H0z" : "M0 0h7L3.5 6z"} fill="currentColor" />
        </svg>
        {up ? change : `-${-change}`}
      </span>
    </Tooltip>
  );
});
