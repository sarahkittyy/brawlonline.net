import type { RoomMode } from "@accounts/types";
import { characterName } from "@common/profile";
import { stageInfo } from "@common/stages";
import type { DiscordActivity, DiscordGame, DiscordGameState } from "@social/discord/types";
import type { QueueMode, QueueState, SocialPlayer, SocialRoom } from "@social/types";
import type { SpectateState } from "@spectate/types";

import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { characterStockKeys } from "@/lib/utils";
import type { Rank } from "@/services/backend/types";

import { DiscordMessages as Messages } from "./discord.messages";

/**
 * The player's Discord activity (Settings > Social; the mockups Sarah chose, 2026-11-24): what the
 * launcher knows of the game, in two lines under "Brawl Online". Discord's member list shows the
 * first line ("Playing Ranked · vs Peachy").
 *
 * The pictures: the stage (big) and the player's character (small) in a game, the Ranked badge
 * while searching and between games, the logo otherwise. Stage and character pictures are the
 * Discord application's art assets (Sarah uploads them by hand: `stage_<P+ stage id>`,
 * `char_<stock key>`, `watching`, `replay`; made from the extracted stage icons and stock icons,
 * docs/design/discord-activity.md); the logo and the rank badges are links to brawlonline.net.
 */

const SITE = "https://brawlonline.net";
export const LOGO_IMAGE = `${SITE}/assets/icon-512.png`;

export function rankImage(rank: Rank): string {
  const file = rank.tier === "legendary" || !rank.division ? rank.tier : `${rank.tier}-${rank.division}`;
  return `${SITE}/assets/ranks/${file}.png`;
}

/** The stage's art asset and its name, for the big picture; none for a stage P+ does not list. */
function stagePicture(id: number | null | undefined): Pick<DiscordActivity, "largeImage" | "largeText"> {
  const info = id != null ? stageInfo(id) : undefined;
  return info ? { largeImage: `stage_${info.id}`, largeText: info.name } : { largeImage: LOGO_IMAGE };
}

/** Characters with no art asset: the transformations, never picked. */
const NO_CHARACTER_ASSET = new Set(["giga_bowser", "wario_man"]);

/** The player's own character in the game being played, for the small picture. */
function ownCharacterPicture(
  game: DiscordGame | null,
  uid: string | undefined,
): Pick<DiscordActivity, "smallImage" | "smallText"> {
  const own = game?.players.find((p) => uid && p.uid === uid) ?? game?.players.find((p) => p.port === game.ownPort);
  const key = own ? characterStockKeys[own.characterId] : undefined;
  if (!key || NO_CHARACTER_ASSET.has(key)) {
    return {};
  }
  return { smallImage: `char_${key}`, smallText: characterName(key) };
}

/** The buttons' links: the website hands them to the launcher (brawlonline://room/join, brawlonline://watch). */
export const joinRoomUrl = (code: string) => `${SITE}/?join=${encodeURIComponent(code)}`;
export const watchGameUrl = (stream: string) => `${SITE}/?watch=${encodeURIComponent(stream)}`;

/** The room the player is in, as the room list has it (public or friends only), or what yourRoom says. */
export type ActivityRoom = {
  name: string | null;
  host: string | null;
  mode: RoomMode;
  ruleset: string | null;
  players: number;
  openSlots: number;
  /** Listed for everyone and an open slot is free: the Join button may show. */
  joinable: boolean;
  code: string;
};

export type ActivityInput = {
  /** The launcher started (Unix ms): the timer while nothing else runs. */
  launcherStartedAt: number;
  /** When the current phase started (Unix ms), for the phases with no time of their own. */
  phaseStartedAt: number;
  you: { uid: string; displayName: string } | null;
  game: DiscordGameState | null;
  queue: QueueState | null;
  /** The 2v2 partner the search was with (the match's queue state names only the opponents). */
  partner: SocialPlayer | null;
  socialRoom: SocialRoom | null;
  room: ActivityRoom | null;
  spectate: SpectateState | null;
  rank: Rank | null;
  showPlayers: boolean;
  buttons: boolean;
};

function modeName(mode: QueueMode): string {
  switch (mode) {
    case "ranked":
      return Messages.ranked();
    case "unranked":
      return Messages.unranked();
    case "direct":
      return Messages.direct();
    case "twos":
      return Messages.twos();
    default:
      return "";
  }
}

function roomModeName(mode: RoomMode): string {
  return mode === "ffa" ? Messages.ffa() : mode === "teams" ? Messages.teams() : Messages.oneVsOne();
}

const isoMs = (iso: string | null | undefined): number | undefined => {
  const ms = iso ? Date.parse(iso) : NaN;
  return Number.isFinite(ms) ? ms : undefined;
};

/** What the game's screen is, in a set: picking characters, picking the stage, or playing. */
type SetScreen = "css" | "sss" | "game" | null;

function setScreen(game: DiscordGameState | null): SetScreen {
  if (!game) {
    return null;
  }
  if (game.game && game.screen === "match") {
    return "game";
  }
  if (game.scene === "scSelCharacter" || game.scene === "scSelCharacter2") {
    return "css";
  }
  if (game.scene === "scSelStage") {
    return "sss";
  }
  return game.scene === "scMelee" && game.screen === "match" ? "game" : null;
}

/** The other side's names in the game being played (its INFO), the player's side left out. */
function opponentsOf(
  game: DiscordGame | null,
  uid: string | undefined,
): { opponents: string[]; partner: string | null } {
  if (!game || !uid) {
    return { opponents: [], partner: null };
  }
  const own = game.players.find((p) => p.uid === uid) ?? game.players.find((p) => p.port === game.ownPort);
  const others = game.players.filter((p) => p !== own);
  if (own && game.isTeams && own.teamId != null) {
    return {
      opponents: others.filter((p) => p.teamId !== own.teamId).map((p) => p.displayName),
      partner: others.find((p) => p.teamId === own.teamId)?.displayName ?? null,
    };
  }
  return { opponents: others.map((p) => p.displayName), partner: null };
}

function joinNames(names: string[]): string {
  return names.length === 2 ? Messages.pair(names[0], names[1]) : names.join(", ");
}

/** A set's line: "Game 1 · Character Select", "Game 2 · 1 - 0", "Game 3 · 1 - 1 · Stage Select". */
function setLine(number: number, wins: [number, number] | null, screen: SetScreen, partner: string | null): string {
  const parts: string[] = [];
  if (partner) {
    parts.push(Messages.withPartner(partner));
  }
  parts.push(Messages.game(number));
  if (wins && number > 1) {
    parts.push(Messages.score(wins[0], wins[1]));
  }
  if (screen === "css") {
    parts.push(Messages.characterSelect());
  } else if (screen === "sss") {
    parts.push(Messages.stageSelect());
  }
  return parts.join(" · ");
}

function watchedLine(names: string[], showPlayers: boolean): string | undefined {
  if (!showPlayers || names.length < 2) {
    return undefined;
  }
  return names.length === 2 ? Messages.versus(names[0], names[1]) : names.join(", ");
}

/** The activity for what the launcher knows now; null shows none (Discord clears it). */
export function buildDiscordActivity(input: ActivityInput): DiscordActivity {
  const { game, queue, spectate, showPlayers } = input;
  const base: DiscordActivity = { largeImage: LOGO_IMAGE };

  // Spectating a live game (the playback Dolphin plays it).
  const spectating =
    spectate &&
    spectate.entry &&
    game?.playbackOpen &&
    (spectate.status === "watching" || spectate.status === "connecting" || spectate.status === "reconnecting");
  if (spectating && spectate.entry) {
    const entry = spectate.entry;
    return {
      ...base,
      details: Messages.watching(),
      state: watchedLine(
        entry.players.map((p) => p.displayName),
        showPlayers,
      ),
      ...stagePicture(entry.game.stageId),
      smallImage: "watching",
      smallText: Messages.spectate(),
      startedAt: isoMs(entry.game.startedAt) ?? input.phaseStartedAt,
    };
  }

  if (game?.netplayOpen && game.otherGame) {
    if (game.otherGame === "Project Punch") {
      return { ...base, details: Messages.projectPunch(), startedAt: input.phaseStartedAt };
    }
    return { ...base, details: Messages.otherGames(), state: game.otherGame, startedAt: input.phaseStartedAt };
  }

  if (game?.netplayOpen) {
    const screen = setScreen(game);
    const inGame = screen === "game" ? game.game : null;
    const gameStart = isoMs(inGame?.startTime);

    // A Direct, Unranked, Ranked or 2v2 set.
    if (queue?.state === "playing") {
      const mode = modeName(queue.mode);
      const fromGame = opponentsOf(inGame, input.you?.uid);
      const opponents = queue.opponents.length > 0 ? queue.opponents.map((o) => o.displayName) : fromGame.opponents;
      const partner = queue.mode === "twos" ? fromGame.partner ?? input.partner?.displayName ?? null : null;
      const activity: DiscordActivity = {
        ...base,
        details: showPlayers && opponents.length > 0 ? Messages.modeVs(mode, joinNames(opponents)) : mode,
        state: setLine(queue.game, queue.wins, screen, showPlayers ? partner : null),
        ...(inGame ? stagePicture(inGame.stageId) : {}),
        ...(inGame ? ownCharacterPicture(inGame, input.you?.uid) : {}),
        startedAt: gameStart ?? input.phaseStartedAt,
      };
      if (!inGame && queue.mode === "ranked" && input.rank) {
        activity.smallImage = rankImage(input.rank);
        activity.smallText = RankBadgeMessages.rankName(input.rank);
      }
      if (input.buttons && queue.liveId) {
        activity.buttons = [{ label: Messages.watchTheirGame(), url: watchGameUrl(queue.liveId) }];
      }
      return activity;
    }

    if (queue?.state === "searching" && queue.mode !== ("rooms" as QueueMode)) {
      const activity: DiscordActivity = {
        ...base,
        details: modeName(queue.mode),
        state: Messages.searching(),
        startedAt: isoMs(queue.since) ?? input.phaseStartedAt,
      };
      if (queue.mode === "ranked" && input.rank) {
        activity.smallImage = rankImage(input.rank);
        activity.smallText = RankBadgeMessages.rankName(input.rank);
      }
      return activity;
    }

    // A room (not a 2v2 party: that is the 2v2 search's).
    const socialRoom = input.socialRoom && !input.socialRoom.twos ? input.socialRoom : null;
    if (socialRoom) {
      const room = input.room;
      const mode = roomModeName(room?.mode ?? socialRoom.mode);
      const name = room?.name ?? (room?.host ? Messages.defaultRoomName(room.host) : undefined);
      const playing = inGame != null || socialRoom.status === "in-game";
      const activity: DiscordActivity = {
        ...base,
        details: name ?? mode,
        state: playing
          ? [name ? mode : null, inGame && inGame.gameIndex >= 1 ? Messages.game(inGame.gameIndex) : null]
              .filter(Boolean)
              .join(" · ") || undefined
          : [name ? mode : null, room?.ruleset ?? null].filter(Boolean).join(" · ") || undefined,
        ...(inGame ? stagePicture(inGame.stageId) : {}),
        ...(inGame ? ownCharacterPicture(inGame, input.you?.uid) : {}),
        startedAt: gameStart ?? input.phaseStartedAt,
        party: { size: socialRoom.players, max: socialRoom.openSlots },
      };
      if (input.buttons && room?.joinable && socialRoom.joinable) {
        activity.buttons = [{ label: Messages.joinTheirRoom(), url: joinRoomUrl(room.code) }];
      }
      return activity;
    }

    if (game.screen === "single-player") {
      return { ...base, details: Messages.training(), startedAt: input.phaseStartedAt };
    }

    // The game's menus: "Brawl Online" and the timer.
    return { ...base, startedAt: input.phaseStartedAt };
  }

  // A replay in the playback Dolphin.
  if (game?.playbackOpen && game.replay) {
    return {
      ...base,
      details: Messages.watching(),
      state: watchedLine(
        game.replay.players.map((p) => p.displayName),
        showPlayers,
      ),
      ...stagePicture(game.replay.stageId),
      smallImage: "replay",
      smallText: Messages.replays(),
      startedAt: input.phaseStartedAt,
    };
  }

  return { ...base, details: Messages.inTheLauncher(), startedAt: input.launcherStartedAt };
}

/**
 * Which phase the activity shows, for its timer: a new phase starts the timer again (searching,
 * a set's game, a room, a replay...). Games and searches carry their own start; this is the rest.
 */
export function activityPhase(a: DiscordActivity): string {
  return `${a.details ?? ""}|${a.state ?? ""}|${a.largeText ?? ""}`;
}
