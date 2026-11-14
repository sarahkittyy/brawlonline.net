import { countryName } from "@common/profile";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import SearchRoundedIcon from "@mui/icons-material/SearchRounded";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import type { SocialPlayer } from "@social/types";
import { clsx } from "clsx";
import React, { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";

import { Flag } from "@/components/flag/flag";
import { useHasStockIcons } from "@/components/profile/character_icon";
import { AdminMessages } from "@/components/social/admin.messages";
import { AddFriendIcon, FriendsIcon } from "@/components/social/friend_icons";
import { InviteButton, PlayerName } from "@/components/social/invite_button";
import type { PlayerMenuTarget } from "@/components/social/player_menu";
import { PlayerMenu } from "@/components/social/player_menu";
import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useAccount, useIsAdmin } from "@/lib/hooks/use_account";
import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useAdminRefresh } from "@/lib/social/admin_refresh";
import { useFriendsLiveGames } from "@/lib/social/friend_activity";
import { useFriendUids } from "@/lib/social/use_friends";
import { useRoomList } from "@/lib/social/use_room_list";
import { useSocial } from "@/lib/social/use_social";
import { useServices } from "@/services";
import type { AdminPlayer } from "@/services/backend/types";

import { ContentBlock } from "../content_block/content_block";
import { FriendsList } from "./friends_list";
import { MainSlot } from "./main_slot";
import styles from "./online_players.module.css";

/** The tab shown, remembered on this computer. */
const TAB_KEY = "ONLINE_PANEL_TAB";
type PanelTab = "online" | "friends";

/** A name and code, with a friend's glyph after them when there is no main column (no disc). */
const NameCell = ({ player, friend, mark }: { player: SocialPlayer; friend: boolean; mark: boolean }) =>
  friend && mark ? (
    <span className={styles.nameWithMark}>
      <PlayerName player={player} />
      <span className={styles.friendAfterName} title={Messages.friend()} data-testid="online-friend">
        <FriendsIcon style={{ fontSize: 13 }} />
      </span>
    </span>
  ) : (
    <PlayerName player={player} />
  );

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
 * The panel under the rating (docs/social-protocol.md, docs/design/friends.md): two tabs, Online
 * and Friends, each with its players online after a green dot; the Friends tab's requests in amber.
 *
 * Online: everyone online in the launcher, the player's friends first (each with the friends glyph
 * on their main's icon, in the dot's place, or after the code without a disc). A name opens a chat
 * tab, the envelope invites, a right click opens the player's menu. With a disc set up, each name
 * has the player's main before it (MainSlot); without one there are no stock icons and the list has
 * no main column. When any flagged player's name and code no longer fit beside the flag, every
 * flag hides (all at once, so a missing flag never reads as "has none"), the envelope stays at the
 * right, and names that still don't fit end in "…"; the list never scrolls sideways. On an admin
 * account, offline players follow the online ones (dimmed, no envelope; banned ones struck
 * through), and the search finds them too.
 *
 * Friends: FriendsList. Its person+ button opens a box for a friend's code under the header.
 *
 * The magnifier opens a search box under the header that filters the tab's list by name or code
 * as you type (Esc or the X closes it).
 */
export const OnlinePlayers = React.memo(function OnlinePlayers() {
  const user = useAccount((s) => s.user);
  const hasPlayKey = useAccount((s) => s.userData?.playKey != null);
  const status = useSocial((s) => s.state.status);
  const message = useSocial((s) => s.state.message);
  const online = useSocial((s) => s.state.online);
  const ignored = useSocial((s) => s.state.ignored);
  const incoming = useSocial((s) => s.state.friends.incoming.filter((p) => !s.state.ignored.includes(p.uid)).length);
  const openTab = useSocial((s) => s.openTab);
  const friendUids = useFriendUids();
  const hasStockIcons = useHasStockIcons();
  const [tab, setTab] = useLocalStorage<PanelTab>(TAB_KEY, "online");
  const [menu, setMenu] = useState<PlayerMenuTarget | null>(null);
  const [searching, setSearching] = useState(false);
  const [adding, setAdding] = useState(false);
  const [query, setQuery] = useState("");
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLUListElement>(null);
  const [flagsFit, setFlagsFit] = useState(true);
  const { query: rooms } = useRoomList();

  const ready = user != null && hasPlayKey && status === "online";
  const friendsTab = tab === "friends";
  const live = useFriendsLiveGames(ready && friendsTab);
  const needle = query.trim().toLowerCase();
  const shown = useMemo(() => {
    const list = needle === "" ? online : online.filter((p) => matches(p, needle));
    // Friends first, each part in the list's own (random) order.
    return [...list.filter((p) => friendUids.has(p.uid)), ...list.filter((p) => !friendUids.has(p.uid))];
  }, [online, needle, friendUids]);
  const offline = useOfflinePlayers(ready && !friendsTab, needle, online, user?.uid);
  const friendsOnline = online.filter((p) => friendUids.has(p.uid)).length;

  // Re-checked when the list's width changes (the window, the column) and when the rows change.
  const listShown = ready && !friendsTab && shown.length + offline.length > 0;
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

  const closeSearch = () => {
    setSearching(false);
    setQuery("");
    inputRef.current?.blur();
  };

  const switchTab = (next: PanelTab) => {
    setTab(next);
    if (next === "online") {
      setAdding(false);
    }
  };

  const heading = (
    <>
      <div className={styles.tabs} role="tablist">
        {(["online", "friends"] as const).map((t) => (
          <button
            key={t}
            type="button"
            role="tab"
            className={styles.tab}
            aria-selected={tab === t}
            data-active={tab === t ? "true" : undefined}
            data-testid={`online-tab-${t}`}
            onClick={() => switchTab(t)}
          >
            <span className={styles.tabLabel}>{t === "online" ? Messages.online() : Messages.friends()}</span>
            {ready && (
              <span
                className={styles.tabCount}
                title={Messages.playersOnline(t === "online" ? online.length : friendsOnline)}
              >
                {t === "online" ? online.length : friendsOnline}
              </span>
            )}
            {t === "friends" && ready && incoming > 0 && (
              <span className={styles.badge} data-testid="friends-requests-badge">
                {incoming}
              </span>
            )}
          </button>
        ))}
      </div>
      {ready && (
        <div className={styles.headIcons}>
          <Tooltip
            title={searching ? Messages.closeSearch() : friendsTab ? Messages.searchFriends() : Messages.searchOnline()}
            placement="top"
            disableInteractive={true}
          >
            <IconButton
              size="small"
              aria-label={searching ? Messages.closeSearch() : Messages.searchOnline()}
              className={clsx(styles.iconButton, searching && styles.iconButtonOn)}
              data-testid="online-search-toggle"
              // Keeps the box focused, so its blur (closing an empty search) doesn't race this click.
              onMouseDown={(e) => e.preventDefault()}
              onClick={() => {
                if (searching) {
                  closeSearch();
                } else {
                  setAdding(false);
                  setSearching(true);
                  requestAnimationFrame(() => inputRef.current?.focus());
                }
              }}
            >
              {searching ? <CloseRoundedIcon fontSize="small" /> : <SearchRoundedIcon fontSize="small" />}
            </IconButton>
          </Tooltip>
          {friendsTab && (
            <Tooltip
              title={adding ? Messages.closeAddFriend() : Messages.addFriendByCode()}
              placement="top"
              disableInteractive={true}
            >
              <IconButton
                size="small"
                aria-label={Messages.addFriendByCode()}
                className={clsx(styles.iconButton, adding && styles.iconButtonOn)}
                data-testid="friends-add-toggle"
                onClick={() => {
                  if (!adding) {
                    closeSearch();
                  }
                  setAdding(!adding);
                }}
              >
                <AddFriendIcon fontSize="small" />
              </IconButton>
            </Tooltip>
          )}
        </div>
      )}
    </>
  );

  const toolbar = !ready ? undefined : searching ? (
    <div className={styles.boxLine} style={{ width: "100%" }}>
      <input
        ref={inputRef}
        className={styles.box}
        type="text"
        value={query}
        placeholder={friendsTab ? Messages.searchFriends() : Messages.searchOnline()}
        aria-label={friendsTab ? Messages.searchFriends() : Messages.searchOnline()}
        spellCheck={false}
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
  ) : friendsTab && adding ? (
    <AddFriendBox onClose={() => setAdding(false)} />
  ) : undefined;

  return (
    <div className={styles.block}>
      <ContentBlock fill={true} heading={heading} toolbar={toolbar} content={renderBody()} />
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
    if (friendsTab) {
      return (
        <FriendsList
          needle={needle}
          rawQuery={query}
          hasStockIcons={hasStockIcons}
          rooms={rooms.data}
          live={live.data?.games}
          onMenu={setMenu}
        />
      );
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
        {shown.map((player) => {
          const friend = friendUids.has(player.uid);
          return (
            <li key={player.uid}>
              <div
                role="button"
                tabIndex={0}
                className={clsx(styles.row, ignored.includes(player.uid) && styles.ignored)}
                data-flagged={player.country ? "" : undefined}
                data-friend={friend ? "" : undefined}
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
                {hasStockIcons && <MainSlot main={player.main} friend={friend} />}
                <NameCell player={player} friend={friend} mark={!hasStockIcons} />
                {player.country && (
                  <span className={styles.flag}>
                    <Flag code={player.country} height={12} title={countryName(player.country)} />
                  </span>
                )}
                <InviteButton player={player} className={player.country ? styles.afterFlag : undefined} />
              </div>
            </li>
          );
        })}
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

/**
 * The Friends tab's box for a friend's code (or profile link), under the header: Send asks them;
 * what happened shows under it. The person+ button or Esc closes it.
 */
const AddFriendBox = ({ onClose }: { onClose: () => void }) => {
  const [code, setCode] = useState("");
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<{ ok: boolean; text: string } | null>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  useEffect(() => inputRef.current?.focus(), []);

  const send = async () => {
    const typed = code.trim();
    if (!typed || busy) {
      return;
    }
    setBusy(true);
    try {
      const r = await window.electron.social.sendFriendRequest(typed);
      if (!r.ok) {
        setResult({ ok: false, text: r.message });
        return;
      }
      setCode("");
      setResult({
        ok: true,
        text:
          r.status === "friends"
            ? Messages.friendsWith(r.player.displayName)
            : Messages.requestSentTo(r.player.displayName, r.player.connectCode),
      });
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className={styles.boxRow} data-testid="friends-add-box">
      <div className={styles.boxLine}>
        <input
          ref={inputRef}
          className={styles.box}
          type="text"
          value={code}
          placeholder={Messages.codePlaceholder()}
          aria-label={Messages.addFriendByCode()}
          spellCheck={false}
          maxLength={200}
          data-testid="friends-add-code"
          onChange={(e) => {
            setCode(e.target.value);
            setResult(null);
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              e.preventDefault();
              void send();
            } else if (e.key === "Escape") {
              e.preventDefault();
              onClose();
            }
          }}
        />
        <Button
          variant="contained"
          size="small"
          disabled={busy || code.trim() === ""}
          data-testid="friends-add-send"
          onClick={() => void send()}
        >
          {Messages.send()}
        </Button>
        <IconButton size="small" aria-label={Messages.closeAddFriend()} className={styles.iconButton} onClick={onClose}>
          <CloseRoundedIcon fontSize="small" />
        </IconButton>
      </div>
      {result && (
        <span className={result.ok ? styles.boxOk : styles.boxError} data-testid="friends-add-result">
          {result.text}
        </span>
      )}
    </div>
  );
};
