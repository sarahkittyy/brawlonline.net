import type { MatchHistoryItem, MatchHistoryPlayer, RankedProfile, RoomMode } from "@/services/backend/types";

import { RankedFormatMessages as Messages } from "./ranked_format.messages";

/** A rating as the launcher shows it everywhere: one decimal. */
export function formatRating(rating: number): string {
  return rating.toFixed(1);
}

/** What a rating shows while its player is placing: nobody sees it until they place (the user's, 2026-10-21). */
export const HIDDEN_RATING = "????";

/** A player's own rating: `1523.4`, or `????` while placing. */
export function formatProfileRating(profile: Pick<RankedProfile, "rating" | "setsToPlace">): string {
  return (profile.setsToPlace ?? 0) > 0 ? HIDDEN_RATING : formatRating(profile.rating);
}

/** Rounds to the one decimal shown, so the sign always matches the digits. */
function roundTenth(n: number): number {
  return Math.round(n * 10) / 10;
}

/** `+16.0`, `-12.3`, `±0.0`. */
export function formatRatingChange(change: number): string {
  const r = roundTenth(change);
  const sign = r > 0 ? "+" : r < 0 ? "-" : "±";
  return `${sign}${Math.abs(r).toFixed(1)}`;
}

/** Rating gains and losses (MUI's light success and error tones, readable on the dark surfaces). */
export const RATING_UP_COLOR = "#66bb6a";
export const RATING_DOWN_COLOR = "#ef5350";

/** The text color of a rating change: green up, red down. */
export function ratingChangeColor(change: number): string | undefined {
  const r = roundTenth(change);
  if (r > 0) {
    return RATING_UP_COLOR;
  }
  if (r < 0) {
    return RATING_DOWN_COLOR;
  }
  return undefined;
}

/** Set wins and losses: `12-3`. */
export function formatRecord(wins: number, losses: number): string {
  return `${wins}-${losses}`;
}

/** `#23 of 137`, or null while placing. */
export function formatPosition(position: number | null | undefined, total: number | null | undefined): string | null {
  if (position == null || position < 1) {
    return null;
  }
  return total ? `#${position} of ${total}` : `#${position}`;
}

/** The mode as the history shows it; a room's game by how the room played (`roomMode`). */
export function modeLabel(mode: MatchHistoryItem["mode"], roomMode?: RoomMode): string {
  switch (mode) {
    case "ranked":
      return Messages.ranked();
    case "direct":
      return Messages.direct();
    case "room":
      return roomMode === "ffa" ? Messages.ffa() : roomMode === "teams" ? Messages.teams() : Messages.oneVsOne();
    default:
      return Messages.unranked();
  }
}

/** A room's team colours (0 red, 1 blue, 2 green), as on the game's character select. */
export const TEAM_COLORS = ["#ef5350", "#42a5f5", "#66bb6a"] as const;

export function teamName(team: number): string {
  return [Messages.redTeam, Messages.blueTeam, Messages.greenTeam][team]?.() ?? "";
}

/** A room game's place (1-4): `1st`, `2nd`, `3rd`, `4th`. */
export function formatPlace(place: number): string {
  return ["1st", "2nd", "3rd"][place - 1] ?? `${place}th`;
}

export function playerName(p: Pick<MatchHistoryPlayer, "displayName">): string {
  return p.displayName || Messages.unknownPlayer();
}

/** The players with `myUid` first (the server keeps mm's order). */
export function orderPlayers(players: readonly MatchHistoryPlayer[], myUid: string | undefined): MatchHistoryPlayer[] {
  const mine = players.filter((p) => p.uid === myUid);
  return [...mine, ...players.filter((p) => p.uid !== myUid)];
}

/** Games won, in the order given: `2 - 1`. */
export function formatScore(players: readonly MatchHistoryPlayer[]): string {
  return players.map((p) => p.wins).join(" - ");
}

/**
 * A row's two sides: mine (me first) and the others. A room's team battle puts my teammates on
 * my side; a room's others are in place order (unplaced last), then by slot.
 */
export function matchSides(
  item: Pick<MatchHistoryItem, "mode" | "room" | "players">,
  myUid: string | undefined,
): [MatchHistoryPlayer[], MatchHistoryPlayer[]] {
  const [me, ...rest] = orderPlayers(item.players, myUid);
  if (!me) {
    return [[], []];
  }
  if (item.mode !== "room" || !item.room || item.room.mode === "1v1") {
    return [[me], rest];
  }
  const byPlace = (a: MatchHistoryPlayer, b: MatchHistoryPlayer) =>
    (a.place ?? 99) - (b.place ?? 99) || (a.port ?? 99) - (b.port ?? 99);
  const teammate = (p: MatchHistoryPlayer) => item.room?.mode === "teams" && p.team != null && p.team === me.team;
  return [[me, ...rest.filter(teammate).sort(byPlace)], rest.filter((p) => !teammate(p)).sort(byPlace)];
}

/**
 * The score column: games won for a 1v1 (`2 - 1`, mine first); a free-for-all's place (`2nd`);
 * a team battle's result (Won, Lost). A draw (every player who stayed first) is Draw; `–` before
 * the players' reports agree on the places, or when I left the game.
 */
export function formatMatchScore(
  item: Pick<MatchHistoryItem, "mode" | "room" | "players">,
  myUid: string | undefined,
): string {
  const players = orderPlayers(item.players, myUid);
  if (item.mode !== "room" || !item.room || item.room.mode === "1v1") {
    return formatScore(players);
  }
  const place = players[0]?.uid === myUid ? players[0]?.place : null;
  if (place == null) {
    return "–";
  }
  if (players.every((p) => p.place == null || p.place === 1)) {
    return Messages.draw();
  }
  if (item.room.mode === "teams") {
    return place === 1 ? Messages.won() : Messages.lost();
  }
  return formatPlace(place);
}

/**
 * A short note for a ranked set that did not simply finish: in progress, abandoned (by whom),
 * or void (why); for a room's game, the room. Null for a finished set and for Unranked and
 * Direct matches.
 */
export function matchNote(
  item: Pick<MatchHistoryItem, "ranked" | "status" | "endReason" | "players"> & Pick<Partial<MatchHistoryItem>, "room">,
): string | null {
  if (item.room) {
    return Messages.room(item.room.code);
  }
  if (!item.ranked) {
    return null;
  }
  switch (item.status) {
    case "COMPLETE":
      return null;
    case "ASSIGNED":
      return Messages.inProgress();
    case "ABANDONED": {
      const uid = item.endReason?.match(/^abandoned by (\S+)$/)?.[1];
      const leaver = uid ? item.players.find((p) => p.uid === uid) : undefined;
      return leaver ? Messages.abandonedBy(playerName(leaver)) : Messages.abandoned();
    }
    case "TERMINATED":
      // A game aborted for its match quality (the server's MATCH_QUALITY_LOW).
      return item.endReason === "match quality low" ? Messages.voidMatchQuality() : Messages.voidConnection();
    case "ERROR":
      return Messages.voidDisagreement();
    case "ORPHANED":
      return Messages.voidNoResult();
    default:
      return Messages.voidOther();
  }
}
