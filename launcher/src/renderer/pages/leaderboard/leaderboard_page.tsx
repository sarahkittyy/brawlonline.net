import { countryName } from "@common/profile";
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
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useEffect, useRef, useState } from "react";

import { Flag } from "@/components/flag/flag";
import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { CharacterIcon } from "@/components/profile/character_icon";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { SocialMessages } from "@/components/social/social.messages";
import { UserName } from "@/components/user_name/user_name";
import { useAccount } from "@/lib/hooks/use_account";
import type { CursorPage } from "@/lib/hooks/use_cursor_pages";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import { useSearchText } from "@/lib/hooks/use_search_text";
import { formatRating, formatRecord, RATING_DOWN_COLOR, RATING_UP_COLOR } from "@/lib/ranked_format";
import { MatchHistoryPageMessages as MatchHistoryMessages } from "@/pages/match_history/match_history_page.messages";
import { useServices } from "@/services";
import type { LeaderboardEntry } from "@/services/backend/types";

import { LeaderboardPageMessages as Messages } from "./leaderboard_page.messages";
import styles from "./leaderboard_page.module.css";

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
/** The element `el` scrolls in. */
const scrollerOf = (el: HTMLElement): HTMLElement | null => {
  for (let p = el.parentElement; p; p = p.parentElement) {
    const { overflowY } = getComputedStyle(p);
    if (overflowY === "auto" || overflowY === "scroll") {
      return p;
    }
  }
  return null;
};

export const LeaderboardPage = React.memo(function LeaderboardPage() {
  const { backendService } = useServices();
  const myUid = useAccount((s) => s.user?.uid);
  const myCode = useAccount((s) => s.userData?.playKey?.connectCode);
  const [text, setText] = useState("");
  const search = useSearchText(text);
  // All, or the player and their friends (docs/design/launcher-redesign.md §7).
  const [scope, setScope] = useState<"all" | "friends">("all");
  const friendsBoard = useQuery({
    queryKey: ["friendsLeaderboard", myUid],
    queryFn: () => backendService.fetchFriendsLeaderboard(),
    enabled: scope === "friends" && myUid != null,
  });
  // The player's own row, pinned under the list: their code searched (positions are the whole board's).
  const myRow = useQuery({
    queryKey: ["leaderboardMe", myCode],
    queryFn: async () => {
      const page = await backendService.fetchLeaderboard(undefined, myCode);
      return page.entries.find((e) => e.uid === myUid) ?? null;
    },
    enabled: myCode != null && myUid != null,
    staleTime: SEARCH_STALE_MS,
  });

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
  const needle = search.toLowerCase();
  const friendsRows = (friendsBoard.data?.entries ?? []).filter(
    (e) => !needle || e.displayName.toLowerCase().includes(needle) || e.connectCode.toLowerCase().includes(needle),
  );
  const rows = scope === "friends" ? friendsRows : pages.items;
  const me = myRow.data;
  const meListed = rows.some((e) => e.uid === myUid);
  // The player's own row in the list is sticky (Sarah's board): under the column heads when the list is scrolled
  // below it, at the list's foot when it is still below; in its place while it is on screen, so it slides in and out
  // of the list at either edge. Stuck, a line splits it from the rows (Chromium 132 has no stuck queries in CSS: this
  // finds where its place is). Not loaded yet, it is always below: pinned under the list instead.
  const listRef = useRef<HTMLDivElement>(null);
  const [stuck, setStuck] = useState<"top" | "bottom" | null>(null);
  useEffect(() => {
    const list = listRef.current;
    const row = meListed ? list?.querySelector<HTMLElement>("tbody tr[data-me]") : null;
    const scroller = row ? scrollerOf(row) : null;
    if (!list || !row || !scroller) {
      setStuck(null);
      return;
    }
    let frame = 0;
    const check = () => {
      frame = 0;
      const head = list.querySelector("thead")?.getBoundingClientRect().height ?? 0;
      list.style.setProperty("--leaderboard-head", `${head}px`);
      const view = scroller.getBoundingClientRect();
      // Its place: under the row before it (a sticky row's own box is where it is stuck).
      const before = row.previousElementSibling ?? row.parentElement;
      const top = before
        ? before === row.parentElement
          ? before.getBoundingClientRect().top
          : before.getBoundingClientRect().bottom
        : 0;
      const bottom = top + row.offsetHeight;
      setStuck(top < view.top + head - 0.5 ? "top" : bottom > view.bottom + 0.5 ? "bottom" : null);
    };
    check();
    const onChange = () => {
      if (!frame) {
        frame = requestAnimationFrame(check);
      }
    };
    scroller.addEventListener("scroll", onChange, { passive: true });
    const resized = typeof ResizeObserver === "undefined" ? null : new ResizeObserver(onChange);
    resized?.observe(scroller);
    return () => {
      scroller.removeEventListener("scroll", onChange);
      resized?.disconnect();
      cancelAnimationFrame(frame);
    };
  }, [meListed, rows]);

  return (
    <div className={styles.page}>
      <div className={styles.head}>
        <div className={styles.titleBlock}>
          <h1 className={styles.title}>{Messages.leaderboard()}</h1>
          {total != null && <div className={styles.count}>{Messages.rankedPlayers(total)}</div>}
        </div>
        {myUid && (
          <div className={styles.seg} role="tablist">
            {(["all", "friends"] as const).map((v) => (
              <button
                key={v}
                type="button"
                role="tab"
                aria-selected={scope === v}
                className={scope === v ? styles.segOn : undefined}
                onClick={() => setScope(v)}
                data-testid={`leaderboard-${v}`}
              >
                {v === "all" ? MatchHistoryMessages.all() : SocialMessages.friends()}
              </button>
            ))}
          </div>
        )}
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
          className={styles.search}
        />
      </div>
      <div className={styles.list} ref={listRef}>
        <InfiniteScrollContainer
          onLoadMore={pages.loadMore}
          hasMore={scope === "all" && pages.hasMore && !pages.error}
          isLoading={scope === "all" ? pages.isLoading || pages.isLoadingMore : friendsBoard.isLoading}
          resetKey={`${scope}:${search}`}
        >
          {scope === "all" && pages.error && pages.items.length === 0 ? null : (
            <Table
              size="small"
              stickyHeader={true}
              style={{ opacity: scope === "all" && pages.isPrevious ? 0.5 : 1, transition: "opacity 150ms" }}
            >
              <LeaderboardColumns />
              <LeaderboardHead />
              <TableBody>
                {rows.map((e) => (
                  <LeaderboardRow
                    key={e.uid}
                    entry={e}
                    isMe={e.uid === myUid}
                    stuck={e.uid === myUid ? stuck : undefined}
                  />
                ))}
              </TableBody>
            </Table>
          )}
          {scope === "all" && !pages.isLoading && !pages.error && pages.items.length === 0 && (
            <Typography variant="body2" color="text.secondary" sx={{ padding: 2 }}>
              {search ? Messages.noMatches(search) : Messages.empty()}
            </Typography>
          )}
          {scope === "all" && <PagedListStatus pages={pages} />}
        </InfiniteScrollContainer>
      </div>
      {me && !meListed && !search && (
        <div className={styles.me} data-testid="leaderboard-me">
          <Table size="small">
            <LeaderboardColumns />
            <TableBody>
              <LeaderboardRow entry={me} isMe={true} />
            </TableBody>
          </Table>
        </div>
      )}
    </div>
  );
});

/**
 * The columns' widths, the same in the list and the pinned row under it. The table's layout is fixed (the CSS): sized
 * from its rows, the columns moved left and right as pages of other names and numbers loaded in while scrolling
 * (Sarah's board). Each width holds its widest content (padding included): the position with a change beside it
 * ("1234 ▼-123"), the badge, the main, "9999.9", "999-999"; the player's column takes the rest.
 */
export const LeaderboardColumns = React.memo(function LeaderboardColumns() {
  return (
    <colgroup>
      <col style={{ width: 120 }} />
      <col style={{ width: 88 }} />
      <col />
      <col style={{ width: 72 }} />
      <col style={{ width: 104 }} />
      <col style={{ width: 104 }} />
    </colgroup>
  );
});

export const LeaderboardHead = React.memo(function LeaderboardHead() {
  return (
    <TableHead>
      <TableRow>
        <TableCell>
          <span style={positionStyle}>{Messages.position()}</span>
        </TableCell>
        <TableCell align="center">{Messages.rank()}</TableCell>
        <TableCell>{Messages.player()}</TableCell>
        <TableCell>{Messages.main()}</TableCell>
        <TableCell align="right">{Messages.rating()}</TableCell>
        <TableCell align="right" sx={{ whiteSpace: "nowrap" }}>
          {Messages.record()}
        </TableCell>
      </TableRow>
    </TableHead>
  );
});

export const LeaderboardRow = React.memo(function LeaderboardRow({
  entry,
  isMe,
  stuck,
}: {
  entry: LeaderboardEntry;
  isMe: boolean;
  /** The player's own row in the list, stuck to the list's top or foot (see LeaderboardPage). */
  stuck?: "top" | "bottom" | null;
}) {
  const strong = isMe ? { fontWeight: "bold" } : undefined;
  return (
    <TableRow data-me={isMe || undefined} data-stuck={stuck ?? undefined} className={isMe ? styles.meRow : undefined}>
      <TableCell sx={{ whiteSpace: "nowrap", ...strong }}>
        <span style={positionStyle}>{entry.position}</span>
        <PositionChange change={entry.positionChange} />
      </TableCell>
      <TableCell align="center" sx={{ paddingY: "4px" }}>
        <RankBadge rank={entry.rank} setsToPlace={entry.setsToPlace} size="small" />
      </TableCell>
      <TableCell className={styles.playerCell} sx={strong}>
        {/* The flag, the name and the code in one cell (docs/design/launcher-redesign.md §7). */}
        <span className={styles.player}>
          {entry.country && <Flag code={entry.country} height={13} title={countryName(entry.country)} />}
          <UserName user={entry} showCode={false} profileLink={true} badgePlace="leaderboard" />
          <span className={styles.code} data-testid="leaderboard-code">
            {entry.connectCode}
          </span>
        </span>
      </TableCell>
      <TableCell sx={{ paddingY: "2px" }}>
        {(entry.main || entry.mainIcon) && (
          <CharacterIcon character={entry.main ?? ""} icon={entry.mainIcon} size={24} />
        )}
      </TableCell>
      <TableCell align="right" sx={{ whiteSpace: "nowrap", ...strong }}>
        {formatRating(entry.rating)}
      </TableCell>
      <TableCell align="right" sx={{ whiteSpace: "nowrap", ...strong }}>
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
