import type { MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import { RankedFormatMessages as Messages } from "./ranked_format.messages";

/** A rating as the launcher shows it everywhere: one decimal. */
export function formatRating(rating: number): string {
  return rating.toFixed(1);
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

/** `#23 of 137`, or null before the first rated set. */
export function formatPosition(position: number | null | undefined, total: number | null | undefined): string | null {
  if (position == null || position < 1) {
    return null;
  }
  return total ? `#${position} of ${total}` : `#${position}`;
}

export function modeLabel(mode: MatchHistoryItem["mode"]): string {
  switch (mode) {
    case "ranked":
      return Messages.ranked();
    case "direct":
      return Messages.direct();
    default:
      return Messages.unranked();
  }
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
 * A short note for a ranked set that did not simply finish: in progress, abandoned (by whom),
 * or void (why). Null for a finished set and for Unranked and Direct matches.
 */
export function matchNote(item: Pick<MatchHistoryItem, "ranked" | "status" | "endReason" | "players">): string | null {
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
      return Messages.voidConnection();
    case "ERROR":
      return Messages.voidDisagreement();
    case "ORPHANED":
      return Messages.voidNoResult();
    default:
      return Messages.voidOther();
  }
}
