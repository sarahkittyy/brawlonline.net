import type { RoomList } from "@accounts/types";
import type { DiscordGameState } from "@social/discord/types";
import type { SocialPlayer } from "@social/types";
import { useQuery } from "@tanstack/react-query";
import log from "electron-log";
import { useEffect, useMemo, useRef, useState } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useDiscordActivity, useDiscordButtons, useDiscordShowPlayers } from "@/lib/hooks/use_settings";
import { useSocial } from "@/lib/social/use_social";
import { useSpectateState } from "@/lib/spectate/use_spectate";
import { useServices } from "@/services";

import type { ActivityRoom } from "./discord_activity";
import { activityPhase, buildDiscordActivity } from "./discord_activity";

/** How often the Dolphins' state is read (game-status.json, the playback Dolphin) while the activity shows. */
const GAME_POLL_MS = 3000;
/** How often the room list is read while the player is in a room (its name, ruleset, whether it is listed). */
const ROOM_POLL_MS = 15_000;

const LAUNCHER_STARTED_AT = Date.now();

/**
 * Keeps the player's Discord activity (Settings > Social) up to date: built from the social stream
 * (searches, sets, rooms), the Dolphins' state and Spectate, and sent to the main process, which
 * talks to the Discord app. Off: the activity is cleared.
 */
export const useDiscordPresence = () => {
  const [enabled] = useDiscordActivity();
  const [showPlayers] = useDiscordShowPlayers();
  const [buttons] = useDiscordButtons();
  const user = useAccount((s) => s.user);
  const rank = useAccount((s) => s.userData?.rankedNetplayProfile?.rank ?? null);
  const social = useSocial((s) => s.state);
  const spectate = useSpectateState();
  const { backendService } = useServices();
  const [game, setGame] = useState<DiscordGameState | null>(null);

  useEffect(() => {
    if (!enabled) {
      setGame(null);
      return;
    }
    let stopped = false;
    const poll = () => {
      window.electron.discord
        .gameState()
        .then((state) => !stopped && setGame(state))
        .catch(log.warn);
    };
    poll();
    const timer = setInterval(poll, GAME_POLL_MS);
    return () => {
      stopped = true;
      clearInterval(timer);
    };
  }, [enabled]);

  // The room's name and ruleset: the room list has them (shared with Home's list, read only while in a room).
  const socialRoom = social.room ?? null;
  const roomList = useQuery<RoomList>({
    queryKey: ["publicRooms", user?.uid],
    queryFn: () => backendService.fetchRooms(),
    enabled: enabled && user != null && socialRoom != null && !socialRoom.twos,
    refetchInterval: ROOM_POLL_MS,
    retry: false,
  });
  const room = useMemo((): ActivityRoom | null => {
    if (!socialRoom) {
      return null;
    }
    const listed = roomList.data?.rooms.find((r) => r.code === socialRoom.code);
    const yours = roomList.data?.yourRoom?.code === socialRoom.code ? roomList.data.yourRoom : null;
    return {
      code: socialRoom.code,
      name: listed?.name ?? yours?.name ?? null,
      host: listed?.host ?? (yours?.host ? user?.displayName ?? null : null),
      mode: listed?.mode ?? socialRoom.mode,
      ruleset: listed?.ruleset?.name ?? null,
      players: socialRoom.players,
      openSlots: socialRoom.openSlots,
      joinable: listed != null && !listed.friends && listed.joinable,
    };
  }, [roomList.data, socialRoom, user?.displayName]);

  // A 2v2 search's partner, kept for the match (whose queue state names only the opponents).
  const partner = useRef<SocialPlayer | null>(null);
  const queue = social.queue ?? null;
  if (queue?.state === "searching" && queue.mode === "twos") {
    partner.current = queue.partner ?? null;
  } else if (!queue) {
    partner.current = null;
  }

  // The current phase's start, for the timer.
  const phase = useRef<{ key: string; at: number }>({ key: "", at: Date.now() });
  const sent = useRef<string | null>(null);

  useEffect(() => {
    if (!enabled) {
      if (sent.current !== "") {
        sent.current = "";
        window.electron.discord.setActivity(null).catch(log.warn);
      }
      return;
    }
    const input = {
      launcherStartedAt: LAUNCHER_STARTED_AT,
      phaseStartedAt: phase.current.at,
      you: user ? { uid: user.uid, displayName: user.displayName } : null,
      game,
      queue,
      partner: partner.current,
      socialRoom,
      room,
      spectate,
      rank,
      showPlayers,
      buttons,
    };
    let activity = buildDiscordActivity(input);
    const key = activityPhase(activity);
    if (key !== phase.current.key) {
      phase.current = { key, at: Date.now() };
      activity = buildDiscordActivity({ ...input, phaseStartedAt: phase.current.at });
    }
    const json = JSON.stringify(activity);
    if (json !== sent.current) {
      sent.current = json;
      window.electron.discord.setActivity(activity).catch(log.warn);
    }
  }, [enabled, game, queue, room, socialRoom, spectate, rank, showPlayers, buttons, user]);
};
