import { countryName } from "@common/profile";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import SearchRoundedIcon from "@mui/icons-material/SearchRounded";
import IconButton from "@mui/material/IconButton";
import type { SocialPlayer } from "@social/types";
import { clsx } from "clsx";
import React, { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";

import { Flag } from "@/components/flag/flag";
import { CharacterIcon, useHasStockIcons, useStockIcon } from "@/components/profile/character_icon";
import { AdminMessages } from "@/components/social/admin.messages";
import { InviteButton, PlayerName } from "@/components/social/invite_button";
import type { PlayerMenuTarget } from "@/components/social/player_menu";
import { PlayerMenu } from "@/components/social/player_menu";
import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useAccount, useIsAdmin } from "@/lib/hooks/use_account";
import { useAdminRefresh } from "@/lib/social/admin_refresh";
import { useSocial } from "@/lib/social/use_social";
import { useServices } from "@/services";
import type { AdminPlayer } from "@/services/backend/types";

import { ContentBlock } from "../content_block/content_block";
import styles from "./online_players.module.css";

/**
 * A player's primary main before their name (docs/design/profiles.md §8): its stock icon, or a small
 * dot in the theme accent's hue for a player without one shown (no main, "Show next to name" off, or a
 * character this disc has no stock icon for), so names line up without a hole or a stand-in character.
 */
const MainSlot = ({ main }: { main?: string }) => {
  const url = useStockIcon(main ?? "");
  return (
    <span className={styles.main} data-testid="online-main">
      {main != null && (main === "random" || url != null) ? (
        <CharacterIcon character={main} size={20} />
      ) : (
        <span className={styles.noMain} />
      )}
    </span>
  );
};

/**
 * Whether every flagged row fits in the list with its flag: each row is laid out at its natural width (name and
 * code in full, flag shown) for a moment, and compared with the width a row has. Synchronous, before paint.
 */
function flagsFitIn(list: HTMLElement): boolean {
  const rows = list.querySelectorAll<HTMLElement>("[data-flagged]");
  if (rows.length === 0) {
    return true;
  }
  const room = rows[0].getBoundingClientRect().width;
  list.classList.add(styles.measure);
  try {
    return Array.from(rows).every((row) => row.getBoundingClientRect().width <= room + 0.5);
  } finally {
    list.classList.remove(styles.measure);
  }
}

/** How long the search box waits after a key before asking the server (an admin's offline players). */
const ADMIN_SEARCH_DELAY_MS = 300;
/** An admin's offline players are read again this often while the list is shown. */
const ADMIN_PLAYERS_REFRESH_MS = 60_000;

const matches = (p: SocialPlayer, needle: string) =>
  needle === "" || p.displayName.toLowerCase().includes(needle) || p.connectCode.toLowerCase().includes(needle);

/**
 * An admin account's offline players, after the online ones (the server's most recently seen, or those the search
 * finds among every account); none for anyone else.
 */
function useOfflinePlayers(enabled: boolean, needle: string, online: SocialPlayer[], ownUid?: string) {
  const isAdmin = useIsAdmin();
  const { backendService } = useServices();
  const refresh = useAdminRefresh((s) => s.count);
  const [data, setData] = useState<AdminPlayer[] | null>(null);
  const on = isAdmin && enabled;
  // Read a moment after the search's last key, again after an admin change and every minute; the last list stays
  // shown meanwhile (an answer for an older search is dropped).
  useEffect(() => {
    if (!on) {
      setData(null);
      return;
    }
    let alive = true;
    const read = () => {
      backendService
        .adminPlayers(needle || undefined)
        .then((players) => alive && setData(players))
        .catch(() => {
          // Kept as it was: the next read may work.
        });
    };
    const first = setTimeout(read, needle === "" ? 0 : ADMIN_SEARCH_DELAY_MS);
    const every = setInterval(read, ADMIN_PLAYERS_REFRESH_MS);
    return () => {
      alive = false;
      clearTimeout(first);
      clearInterval(every);
    };
  }, [on, needle, refresh, backendService]);
  return useMemo(() => {
    if (!on || !data) {
      return [];
    }
    const onlineUids = new Set(online.map((p) => p.uid));
    return data
      .filter((p) => p.uid !== ownUid && !onlineUids.has(p.uid))
      .map(({ banned, ...p }) => ({ player: p as SocialPlayer, banned }))
      .filter(({ player }) => matches(player, needle));
  }, [on, data, online, ownUid, needle]);
}

/**
 * Everyone online in the launcher (docs/social-protocol.md), under the rating: a name opens a chat
 * tab, the envelope invites, a right click opens the player's menu. The magnifier next to the
 * title opens a search box in place of the count that filters the list by name or connect code
 * as you type. With a disc set up, each name has the player's main before it (MainSlot); without
 * one there are no stock icons and the list has no main column. When any flagged player's name and code no
 * longer fit beside the flag, every flag hides (all at once, so a missing flag never reads as "has none"), the
 * envelope stays at the right, and names that still don't fit end in "…"; the list never scrolls sideways.
 * On an admin account, offline players follow the online ones (dimmed, no envelope; banned ones struck through),
 * and the search finds them too.
 */
export const OnlinePlayers = React.memo(function OnlinePlayers() {
  const user = useAccount((s) => s.user);
  const hasPlayKey = useAccount((s) => s.userData?.playKey != null);
  const status = useSocial((s) => s.state.status);
  const message = useSocial((s) => s.state.message);
  const online = useSocial((s) => s.state.online);
  const ignored = useSocial((s) => s.state.ignored);
  const openTab = useSocial((s) => s.openTab);
  const hasStockIcons = useHasStockIcons();
  const [menu, setMenu] = useState<PlayerMenuTarget | null>(null);
  const [searching, setSearching] = useState(false);
  const [query, setQuery] = useState("");
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLUListElement>(null);
  const [flagsFit, setFlagsFit] = useState(true);

  const needle = query.trim().toLowerCase();
  const shown = useMemo(() => (needle === "" ? online : online.filter((p) => matches(p, needle))), [online, needle]);
  const offline = useOfflinePlayers(user != null && hasPlayKey && status === "online", needle, online, user?.uid);

  // Re-checked when the list's width changes (the window, the column) and when the rows change.
  const listShown = user != null && hasPlayKey && status === "online" && shown.length + offline.length > 0;
  useLayoutEffect(() => {
    const list = listRef.current;
    if (!list) {
      return;
    }
    const check = () => setFlagsFit(flagsFitIn(list));
    check();
    if (typeof ResizeObserver === "undefined") {
      return;
    }
    const observer = new ResizeObserver(check);
    observer.observe(list);
    return () => observer.disconnect();
  }, [listShown, shown, offline]);

  const canSearch = user != null && hasPlayKey && status === "online";
  const count =
    canSearch && !searching ? <span className={styles.count}>{Messages.playersOnline(online.length)}</span> : undefined;

  const closeSearch = () => {
    setSearching(false);
    setQuery("");
    inputRef.current?.blur();
  };

  const search = canSearch ? (
    <div className={clsx(styles.search, searching && styles.searchOpen)}>
      <IconButton
        size="small"
        aria-label={searching ? Messages.closeSearch() : Messages.searchOnline()}
        title={searching ? Messages.closeSearch() : Messages.searchOnline()}
        className={styles.searchButton}
        data-testid="online-search-toggle"
        // Keeps the box focused, so its blur (closing an empty search) doesn't race this click.
        onMouseDown={(e) => e.preventDefault()}
        onClick={() => {
          if (searching) {
            closeSearch();
          } else {
            setSearching(true);
            inputRef.current?.focus();
          }
        }}
      >
        {searching ? <CloseRoundedIcon fontSize="small" /> : <SearchRoundedIcon fontSize="small" />}
      </IconButton>
      <input
        ref={inputRef}
        className={styles.searchInput}
        type="text"
        value={query}
        placeholder={Messages.searchOnline()}
        aria-label={Messages.searchOnline()}
        spellCheck={false}
        tabIndex={searching ? 0 : -1}
        aria-hidden={!searching}
        data-testid="online-search"
        onChange={(e) => setQuery(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Escape") {
            e.preventDefault();
            closeSearch();
          }
        }}
        onBlur={() => {
          if (query.trim() === "") {
            setSearching(false);
          }
        }}
      />
    </div>
  ) : undefined;

  return (
    <div className={styles.block}>
      <ContentBlock fill={true} title={Messages.online()} titleAction={search} endIcon={count} content={renderBody()} />
      <PlayerMenu target={menu} onClose={() => setMenu(null)} />
    </div>
  );

  function renderBody() {
    if (!user) {
      return <p className={styles.notice}>{Messages.loggedOut()}</p>;
    }
    if (!hasPlayKey) {
      return <p className={styles.notice}>{Messages.setUpOnline()}</p>;
    }
    if (status !== "online") {
      return <p className={styles.notice}>{status === "off" && message ? message : Messages.connecting()}</p>;
    }
    if (online.length === 0 && offline.length === 0) {
      return <p className={styles.notice}>{Messages.nobodyOnline()}</p>;
    }
    if (shown.length === 0 && offline.length === 0) {
      return <p className={styles.notice}>{Messages.noOnlineMatch(query.trim())}</p>;
    }
    return (
      <ul
        ref={listRef}
        className={clsx(styles.list, !flagsFit && styles.hideFlags)}
        data-testid="online-players"
        data-flags={flagsFit ? "shown" : "hidden"}
      >
        {shown.map((player) => (
          <li key={player.uid}>
            <div
              role="button"
              tabIndex={0}
              className={clsx(styles.row, ignored.includes(player.uid) && styles.ignored)}
              data-flagged={player.country ? "" : undefined}
              onClick={() => openTab(player.uid, true)}
              onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  openTab(player.uid, true);
                }
              }}
              onContextMenu={(e) => {
                e.preventDefault();
                setMenu({ player, x: e.clientX, y: e.clientY });
              }}
            >
              {hasStockIcons && <MainSlot main={player.main} />}
              <PlayerName player={player} />
              {player.country && (
                <span className={styles.flag}>
                  <Flag code={player.country} height={12} title={countryName(player.country)} />
                </span>
              )}
              <InviteButton player={player} className={player.country ? styles.afterFlag : undefined} />
            </div>
          </li>
        ))}
        {offline.length > 0 && (
          <li className={styles.offlineHeader} data-testid="online-offline-header">
            {AdminMessages.offline()}
          </li>
        )}
        {offline.map(({ player, banned }) => (
          <li key={player.uid}>
            <div
              role="button"
              tabIndex={0}
              className={clsx(styles.row, styles.offline, banned && styles.banned)}
              data-flagged={player.country ? "" : undefined}
              data-offline=""
              onClick={() => openTab(player.uid, true, player)}
              onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  openTab(player.uid, true, player);
                }
              }}
              onContextMenu={(e) => {
                e.preventDefault();
                setMenu({ player, x: e.clientX, y: e.clientY });
              }}
            >
              {hasStockIcons && <MainSlot main={player.main} />}
              <PlayerName player={player} />
              {player.country && (
                <span className={styles.flag}>
                  <Flag code={player.country} height={12} title={countryName(player.country)} />
                </span>
              )}
            </div>
          </li>
        ))}
      </ul>
    );
  }
});
