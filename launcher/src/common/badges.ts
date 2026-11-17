/**
 * Badges before a player's name and the icon an admin set in place of their primary main (docs/design/profiles.md
 * §10). Only `pp-admin` gives them; the server sends each as an icon id, and the launcher builds the icon's address
 * from it (`/v1/icons/<id>.png` on the accounts server). The server keeps the same places
 * (server/crates/common/src/badges.rs).
 */

/** Where a badge can show. A badge with no places listed shows in all of them. */
export const BADGE_PLACES = ["profile", "hover", "online", "chat", "leaderboard", "rooms"] as const;
export type BadgePlace = (typeof BADGE_PLACES)[number];

export type NameBadge = {
  /** The icon's id. */
  icon: string;
  /** The hover text. */
  label?: string;
  /** Where it shows; absent: everywhere. */
  places?: BadgePlace[];
};

/** The most badges a player has (server: MAX_BADGES). */
export const MAX_BADGES = 8;

/** An icon id: 20 hex digits (the start of the file's SHA-256). */
export function isIconId(v: unknown): v is string {
  return typeof v === "string" && /^[0-9a-f]{20}$/.test(v);
}

/** An icon's address on the accounts server (`base`: its URL, no trailing slash). */
export function iconUrl(base: string, id: string): string {
  return `${base}/v1/icons/${id}.png`;
}

/** Badges as the server sends them, checked: bad entries are dropped, unknown places too. */
export function parseBadges(v: unknown): NameBadge[] | undefined {
  if (!Array.isArray(v)) {
    return undefined;
  }
  const out: NameBadge[] = [];
  for (const b of v.slice(0, MAX_BADGES)) {
    if (!b || typeof b !== "object" || !isIconId(b.icon)) {
      continue;
    }
    const badge: NameBadge = { icon: b.icon };
    if (typeof b.label === "string" && b.label) {
      badge.label = b.label.slice(0, 60);
    }
    if (Array.isArray(b.places)) {
      badge.places = b.places.filter((p: unknown): p is BadgePlace => BADGE_PLACES.includes(p as BadgePlace));
    }
    out.push(badge);
  }
  return out.length > 0 ? out : undefined;
}

/** The badges that show in `place`. */
export function badgesFor(badges: readonly NameBadge[] | null | undefined, place: BadgePlace): NameBadge[] {
  return (badges ?? []).filter((b) => !b.places || b.places.includes(place));
}
