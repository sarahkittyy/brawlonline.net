import type { RoomList } from "@accounts/types";
import { countryName } from "@common/profile";
import ArrowDropDownRoundedIcon from "@mui/icons-material/ArrowDropDownRounded";
import ArrowRightRoundedIcon from "@mui/icons-material/ArrowRightRounded";
import CheckRoundedIcon from "@mui/icons-material/CheckRounded";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import Button from "@mui/material/Button";
import Tooltip from "@mui/material/Tooltip";
import type { Friend, SocialPlayer } from "@social/types";
import type { LiveGameEntry } from "@spectate/types";
import { clsx } from "clsx";
import React, { useState } from "react";

import { Flag } from "@/components/flag/flag";
import { RoomNameMessages } from "@/components/room_name/room_name.messages";
import { AddFriendIcon } from "@/components/social/friend_icons";
import { InviteButton, PlayerName } from "@/components/social/invite_button";
import type { PlayerMenuTarget } from "@/components/social/player_menu";
import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useToasts } from "@/lib/hooks/use_toasts";
import type { FriendActivity } from "@/lib/social/friend_activity";
import { activityOf } from "@/lib/social/friend_activity";
import { joinRoomByCode } from "@/lib/social/join_room";
import { useFriendActions } from "@/lib/social/use_friends";
import { useSocial } from "@/lib/social/use_social";

import { MainSlot } from "./main_slot";
import styles from "./online_players.module.css";

/** Whether the Friends tab's offline friends are shown, remembered on this computer. */
const OFFLINE_OPEN_KEY = "FRIENDS_OFFLINE_OPEN";

const LIVE_MODE: Record<LiveGameEntry["mode"], string> = {
  ranked: "Ranked",
  unranked: "Unranked",
  direct: "Direct",
  room: "a room",
  twos: "2v2",
};

/** A typed connect code, as the search box may hold one: offered as a friend request when no friend matches. */
const CODE = /^[A-Za-z]{1,4}#\d{1,4}$/;

const matches = (p: SocialPlayer, needle: string) =>
  needle === "" || p.displayName.toLowerCase().includes(needle) || p.connectCode.toLowerCase().includes(needle);

/**
 * Home's Friends tab (docs/design/friends.md): requests to the player first (accept / decline),
 * then the friends online with what they are doing (a room the player can see, with JOIN; a game
 * others can watch, with WATCH), then the offline ones (folding), then the player's own requests
 * waiting for an answer (Cancel). The search box filters it; a code that is no friend's offers a
 * request. Left click: a chat tab; right click: the player's menu.
 */
export const FriendsList = ({
  needle,
  rawQuery,
  hasStockIcons,
  rooms,
  live,
  onMenu,
}: {
  needle: string;
  rawQuery: string;
  hasStockIcons: boolean;
  rooms: RoomList | undefined;
  live: LiveGameEntry[] | undefined;
  onMenu: (t: PlayerMenuTarget) => void;
}) => {
  const lists = useSocial((s) => s.state.friends);
  const online = useSocial((s) => s.state.online);
  const ignored = useSocial((s) => s.state.ignored);
  const openTab = useSocial((s) => s.openTab);
  const actions = useFriendActions();
  const [offlineOpen, setOfflineOpen] = useLocalStorage(OFFLINE_OPEN_KEY, true);
  const [sending, setSending] = useState(false);

  // An online friend as the online list has them (their name and main as of now).
  const onlineByUid = new Map(online.map((p) => [p.uid, p]));
  const friendsOnline: SocialPlayer[] = [];
  const friendsOffline: Friend[] = [];
  for (const f of lists.friends) {
    const now = onlineByUid.get(f.uid);
    if (now) {
      friendsOnline.push(now);
    } else {
      friendsOffline.push(f);
    }
  }
  const shownOnline = friendsOnline.filter((p) => matches(p, needle));
  const shownOffline = friendsOffline.filter((p) => matches(p, needle));
  const incoming = needle === "" ? lists.incoming.filter((p) => !ignored.includes(p.uid)) : [];
  const outgoing = needle === "" ? lists.outgoing : [];

  if (!lists.loaded) {
    return <p className={styles.notice}>{Messages.connecting()}</p>;
  }
  if (needle === "" && lists.friends.length + incoming.length + outgoing.length === 0) {
    return <p className={styles.notice}>{Messages.noFriendsYet()}</p>;
  }
  if (needle !== "" && shownOnline.length + shownOffline.length === 0) {
    const code = rawQuery.trim();
    return (
      <div className={styles.notice} data-testid="friends-no-match">
        {Messages.noFriendMatch(code)}
        {CODE.test(code) && (
          <div>
            <Button
              size="small"
              className={styles.sendRequest}
              startIcon={<AddFriendIcon />}
              disabled={sending}
              data-testid="friends-search-request"
              onClick={() => {
                setSending(true);
                void actions.add(code.toUpperCase()).finally(() => setSending(false));
              }}
            >
              {Messages.sendRequestTo(code.toUpperCase())}
            </Button>
          </div>
        )}
      </div>
    );
  }

  const rowProps = (player: SocialPlayer) => ({
    role: "button",
    tabIndex: 0,
    onClick: () => openTab(player.uid, true, player),
    onKeyDown: (e: React.KeyboardEvent) => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        openTab(player.uid, true, player);
      }
    },
    onContextMenu: (e: React.MouseEvent) => {
      e.preventDefault();
      onMenu({ player, x: e.clientX, y: e.clientY });
    },
  });

  return (
    <ul className={styles.list} data-testid="friends-list">
      {incoming.length > 0 && (
        <li className={clsx(styles.section, styles.requests)}>
          {Messages.requestsSection()} <span>· {incoming.length}</span>
        </li>
      )}
      {incoming.map((player) => (
        <li key={player.uid}>
          <div
            className={clsx(styles.row, styles.request)}
            data-testid={`friend-incoming-${player.uid}`}
            {...rowProps(player)}
          >
            {hasStockIcons && <MainSlot main={player.main} />}
            <div className={styles.nameCol}>
              <PlayerName player={player} />
              <span className={styles.status}>{Messages.wantsToBeFriends()}</span>
            </div>
            <Tooltip title={Messages.accept()} placement="top" disableInteractive={true}>
              <button
                type="button"
                className={styles.accept}
                aria-label={Messages.accept()}
                onClick={(e) => {
                  e.stopPropagation();
                  void actions.accept(player);
                }}
              >
                <CheckRoundedIcon style={{ fontSize: 18 }} />
              </button>
            </Tooltip>
            <Tooltip title={Messages.decline()} placement="top" disableInteractive={true}>
              <button
                type="button"
                className={styles.decline}
                aria-label={Messages.decline()}
                onClick={(e) => {
                  e.stopPropagation();
                  void actions.decline(player);
                }}
              >
                <CloseRoundedIcon style={{ fontSize: 18 }} />
              </button>
            </Tooltip>
          </div>
        </li>
      ))}
      {shownOnline.length > 0 && (
        <li className={styles.section}>
          {Messages.onlineSection()} <span className={styles.sectionCount}>· {shownOnline.length}</span>
        </li>
      )}
      {shownOnline.map((player) => (
        <li key={player.uid}>
          <div
            className={clsx(styles.row, ignored.includes(player.uid) && styles.ignored)}
            data-testid={`friend-online-${player.uid}`}
            {...rowProps(player)}
          >
            {hasStockIcons && <MainSlot main={player.main} />}
            <div className={styles.nameCol}>
              <PlayerName player={player} />
              <ActivityLine activity={activityOf(player.uid, rooms?.rooms, rooms?.yourRoom?.code, live)} />
            </div>
            <ActivityAction activity={activityOf(player.uid, rooms?.rooms, rooms?.yourRoom?.code, live)} />
            <InviteButton player={player} />
          </div>
        </li>
      ))}
      {shownOffline.length > 0 && (
        <li className={styles.section}>
          <button
            type="button"
            className={styles.sectionToggle}
            aria-expanded={offlineOpen || needle !== ""}
            data-testid="friends-offline-toggle"
            onClick={() => setOfflineOpen(!offlineOpen)}
          >
            {offlineOpen || needle !== "" ? (
              <ArrowDropDownRoundedIcon style={{ fontSize: 16 }} />
            ) : (
              <ArrowRightRoundedIcon style={{ fontSize: 16 }} />
            )}
            {Messages.offlineSection()} <span className={styles.sectionCount}>· {shownOffline.length}</span>
          </button>
        </li>
      )}
      {(offlineOpen || needle !== "") &&
        shownOffline.map((player) => (
          <li key={player.uid}>
            <div
              className={clsx(styles.row, styles.offline)}
              data-flagged={player.country ? "" : undefined}
              data-testid={`friend-offline-${player.uid}`}
              {...rowProps(player)}
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
      {outgoing.length > 0 && (
        <li className={styles.section}>
          {Messages.sentSection()} <span className={styles.sectionCount}>· {outgoing.length}</span>
        </li>
      )}
      {outgoing.map((player) => (
        <li key={player.uid}>
          <div
            className={clsx(styles.row, styles.offline)}
            data-testid={`friend-outgoing-${player.uid}`}
            {...rowProps(player)}
          >
            {hasStockIcons && <MainSlot main={player.main} />}
            <PlayerName player={player} />
            <button
              type="button"
              className={styles.cancelSent}
              onClick={(e) => {
                e.stopPropagation();
                void actions.cancel(player);
              }}
            >
              {Messages.cancel()}
            </button>
          </div>
        </li>
      ))}
    </ul>
  );
};

/** What an online friend is doing, under their name. */
const ActivityLine = ({ activity }: { activity: FriendActivity }) => {
  switch (activity.kind) {
    case "room": {
      const r = activity.room;
      const name = activity.yours ? Messages.inYourRoom() : r.name ?? RoomNameMessages.defaultName(r.host);
      return (
        <span className={clsx(styles.status, r.status === "waiting" ? styles.statusWaiting : styles.statusPlaying)}>
          {name} · {r.players}/{r.openSlots}
        </span>
      );
    }
    case "game":
      return (
        <span className={clsx(styles.status, styles.statusPlaying)}>
          {Messages.playing(LIVE_MODE[activity.game.mode] ?? "")}
        </span>
      );
    case "online":
      return <span className={styles.status}>{Messages.inTheLauncher()}</span>;
  }
};

/** JOIN a friend's room (FULL when it has no empty slot), WATCH their game. */
const ActivityAction = ({ activity }: { activity: FriendActivity }) => {
  const uid = useSocial((s) => s.state.you);
  const { showError } = useToasts();
  const [busy, setBusy] = useState(false);
  if (activity.kind === "room" && !activity.yours) {
    const room = activity.room;
    return (
      <Tooltip title={room.joinable ? Messages.joinTooltip() : ""} placement="top" disableInteractive={true}>
        <span>
          <button
            type="button"
            className={styles.pill}
            disabled={!room.joinable || busy}
            data-testid={`friend-join-${room.code}`}
            onClick={(e) => {
              e.stopPropagation();
              setBusy(true);
              void joinRoomByCode(room.code).finally(() => setBusy(false));
            }}
          >
            {room.joinable ? Messages.joinShort() : Messages.fullShort()}
          </button>
        </span>
      </Tooltip>
    );
  }
  if (activity.kind === "game" && uid) {
    const game = activity.game;
    return (
      <Tooltip title={Messages.watchTooltip()} placement="top" disableInteractive={true}>
        <button
          type="button"
          className={styles.pill}
          disabled={busy}
          onClick={(e) => {
            e.stopPropagation();
            setBusy(true);
            void window.electron.spectate
              .watch(uid, game.id)
              .then((r) => {
                if (!r.ok) {
                  showError(r.message);
                }
              })
              .catch((err) => showError(err))
              .finally(() => setBusy(false));
          }}
        >
          {Messages.watchShort()}
        </button>
      </Tooltip>
    );
  }
  return null;
};
