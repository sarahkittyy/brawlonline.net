import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Typography from "@mui/material/Typography";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useEffect, useState } from "react";

import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { useAccount } from "@/lib/hooks/use_account";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import {
  formatPosition,
  formatRating,
  formatRatingChange,
  formatScore,
  matchNote,
  modeLabel,
  orderPlayers,
  playerName,
  ratingChangeColor,
} from "@/lib/ranked_format";
import { monthDayHourFormat } from "@/lib/time";
import { useServices } from "@/services";
import type { AuthUser } from "@/services/auth/types";
import type { MatchHistoryFilter, MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import { ProfilePageMessages as Messages } from "./profile_page.messages";

/**
 * The logged-in player's ranked standing and match history. Slippi shows these on slippi.gg;
 * here they are a plain page in the launcher's style.
 */
export const ProfilePage = React.memo(function ProfilePage({ user }: { user: AuthUser }) {
  return (
    <div style={{ display: "flex", flexDirection: "column", flex: 1, minWidth: 0, padding: "20px 20px 0" }}>
      <RankedSummary uid={user.uid} />
      <MatchHistory uid={user.uid} />
    </div>
  );
});

const RankedSummary = ({ uid }: { uid: string }) => {
  const { backendService } = useServices();
  const stored = useAccount((s) => s.userData?.rankedNetplayProfile);
  const playKey = useAccount((s) => s.userData?.playKey);
  const displayName = useAccount((s) => s.displayName);
  // Fresh on every visit (the rating changes after each set); the stored one until it arrives.
  const { data } = useQuery({
    queryKey: ["rankedProfile", uid],
    queryFn: () => backendService.fetchRankedNetplayProfile(uid),
    refetchOnMount: "always",
  });
  const updateRanking = useAccount((s) => s.updateRanking);
  useEffect(() => {
    // Keep Home's "My Ranking" block in step.
    if (data && useAccount.getState().user?.uid === uid) {
      updateRanking(data);
    }
  }, [data, uid, updateRanking]);
  const profile = data ?? stored;
  const position = formatPosition(profile?.position, profile?.rankedPlayers);

  return (
    <div style={{ marginBottom: 16 }}>
      <Typography variant="h5">
        {displayName}
        {playKey?.connectCode && (
          <Typography component="span" color="text.secondary" sx={{ marginLeft: 1.5 }}>
            {playKey.connectCode}
          </Typography>
        )}
      </Typography>
      {profile && (
        <div style={{ display: "flex", alignItems: "baseline", gap: 16, marginTop: 8 }}>
          <span style={{ color: "var(--text-secondary)" }}>{Messages.rating()}</span>
          <span
            data-testid="profile-rating"
            style={{ color: "var(--accent-primary)", fontWeight: "bold", fontSize: 24 }}
          >
            {formatRating(profile.rating)}
          </span>
          <span style={{ color: "var(--text-secondary)" }}>{Messages.setsPlayed(profile.setsPlayed)}</span>
          <span style={{ color: "var(--text-secondary)" }}>{position ?? Messages.notRankedYet()}</span>
        </div>
      )}
    </div>
  );
};

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
  const pages = useCursorPages<MatchHistoryItem>({ queryKey: ["matchHistory", uid, mode], fetchPage });

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
                </TableRow>
              </TableHead>
              <TableBody>
                {pages.items.map((m) => (
                  <MatchRow key={m.matchId} item={m} myUid={uid} />
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

export const MatchRow = React.memo(function MatchRow({ item, myUid }: { item: MatchHistoryItem; myUid: string }) {
  const players = orderPlayers(item.players, myUid);
  const [me, ...others] = players;
  const note = matchNote(item);
  return (
    <TableRow>
      <TableCell>
        <div>{modeLabel(item.mode)}</div>
        {note && <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>{note}</div>}
      </TableCell>
      <TableCell sx={{ whiteSpace: "nowrap" }}>{monthDayHourFormat(new Date(item.createdAt))}</TableCell>
      <TableCell>{me && <PlayerCell player={me} />}</TableCell>
      <TableCell align="center" sx={{ fontWeight: "bold", whiteSpace: "nowrap" }}>
        {formatScore(players)}
      </TableCell>
      <TableCell>
        {others.map((p) => (
          <PlayerCell key={p.uid} player={p} />
        ))}
      </TableCell>
    </TableRow>
  );
});

const PlayerCell = ({ player }: { player: MatchHistoryPlayer }) => {
  return (
    <div>
      <span>{playerName(player)}</span>
      {player.connectCode && (
        <span style={{ marginLeft: 8, color: "var(--text-secondary)" }}>{player.connectCode}</span>
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
