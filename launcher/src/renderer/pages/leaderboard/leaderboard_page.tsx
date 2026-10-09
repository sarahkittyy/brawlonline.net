import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import Typography from "@mui/material/Typography";
import React, { useCallback } from "react";

import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { useAccount } from "@/lib/hooks/use_account";
import type { CursorPage } from "@/lib/hooks/use_cursor_pages";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import { formatRating, formatRecord } from "@/lib/ranked_format";
import { useServices } from "@/services";
import type { LeaderboardEntry } from "@/services/backend/types";

import { LeaderboardPageMessages as Messages } from "./leaderboard_page.messages";

type LeaderboardPageData = CursorPage<LeaderboardEntry> & { total: number };

/**
 * The ranked leaderboard: everyone with a rated set, by Elo rating. Slippi has its leaderboards
 * on slippi.gg rather than in the launcher; this is a plain table in the launcher's style.
 * Pages load as the list scrolls near its end.
 */
export const LeaderboardPage = React.memo(function LeaderboardPage() {
  const { backendService } = useServices();
  const myUid = useAccount((s) => s.user?.uid);

  const fetchPage = useCallback(
    async (after: string | undefined): Promise<LeaderboardPageData> => {
      const page = await backendService.fetchLeaderboard(after);
      return { items: page.entries, next: page.next, total: page.total };
    },
    [backendService],
  );
  const pages = useCursorPages<LeaderboardEntry>({ queryKey: ["leaderboard"], fetchPage });
  const total = (pages.firstPage as LeaderboardPageData | undefined)?.total;

  return (
    <div style={{ display: "flex", flexDirection: "column", flex: 1, minWidth: 0, padding: "20px 20px 0" }}>
      <div style={{ display: "flex", alignItems: "baseline", gap: 12, marginBottom: 12 }}>
        <Typography variant="h5">{Messages.leaderboard()}</Typography>
        {total != null && (
          <Typography variant="body2" color="text.secondary">
            {Messages.rankedPlayers(total)}
          </Typography>
        )}
      </div>
      <div style={{ flex: 1, minHeight: 0 }}>
        <InfiniteScrollContainer
          onLoadMore={pages.loadMore}
          hasMore={pages.hasMore && !pages.error}
          isLoading={pages.isLoading || pages.isLoadingMore}
        >
          {pages.error && pages.items.length === 0 ? null : (
            <Table size="small" stickyHeader={true}>
              <TableHead>
                <TableRow>
                  <TableCell align="right" sx={{ width: 64 }}>
                    {Messages.position()}
                  </TableCell>
                  <TableCell>{Messages.player()}</TableCell>
                  <TableCell>{Messages.connectCode()}</TableCell>
                  <TableCell align="right">{Messages.rating()}</TableCell>
                  <TableCell align="right">{Messages.record()}</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {pages.items.map((e) => (
                  <LeaderboardRow key={e.uid} entry={e} isMe={e.uid === myUid} />
                ))}
              </TableBody>
            </Table>
          )}
          {!pages.isLoading && !pages.error && pages.items.length === 0 && (
            <Typography variant="body2" color="text.secondary" sx={{ padding: 2 }}>
              {Messages.empty()}
            </Typography>
          )}
          <PagedListStatus pages={pages} />
        </InfiniteScrollContainer>
      </div>
    </div>
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
      <TableCell align="right" sx={strong}>
        {entry.position}
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
