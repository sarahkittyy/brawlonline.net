/**
 * What a player can put on their profile (docs/design/profiles.md): mains, a country and social media accounts. The
 * server keeps the same lists and checks them (server/crates/common/src/profile.rs, countries.rs); keep both in step.
 */
import type { SocialPlatform } from "@accounts/types";

import countryList from "./countries.json";

/**
 * P+'s roster as a profile's mains, in the picker's order, plus Random: the stock icon key (brawl_assets/catalog.ts)
 * and name. Giga Bowser, Wario-Man and Brawl's Pokemon Trainer are left out (not picked on P+'s character select).
 */
export const CHARACTERS: readonly { key: string; name: string }[] = [
  { key: "mario", name: "Mario" },
  { key: "luigi", name: "Luigi" },
  { key: "peach", name: "Peach" },
  { key: "bowser", name: "Bowser" },
  { key: "donkey_kong", name: "Donkey Kong" },
  { key: "diddy_kong", name: "Diddy Kong" },
  { key: "yoshi", name: "Yoshi" },
  { key: "wario", name: "Wario" },
  { key: "link", name: "Link" },
  { key: "zelda", name: "Zelda" },
  { key: "sheik", name: "Sheik" },
  { key: "ganondorf", name: "Ganondorf" },
  { key: "toon_link", name: "Toon Link" },
  { key: "samus", name: "Samus" },
  { key: "zero_suit_samus", name: "Zero Suit Samus" },
  { key: "pit", name: "Pit" },
  { key: "ice_climbers", name: "Ice Climbers" },
  { key: "rob", name: "R.O.B." },
  { key: "kirby", name: "Kirby" },
  { key: "meta_knight", name: "Meta Knight" },
  { key: "king_dedede", name: "King Dedede" },
  { key: "olimar", name: "Olimar" },
  { key: "fox", name: "Fox" },
  { key: "falco", name: "Falco" },
  { key: "wolf", name: "Wolf" },
  { key: "captain_falcon", name: "Captain Falcon" },
  { key: "pikachu", name: "Pikachu" },
  { key: "squirtle", name: "Squirtle" },
  { key: "ivysaur", name: "Ivysaur" },
  { key: "charizard", name: "Charizard" },
  { key: "lucario", name: "Lucario" },
  { key: "jigglypuff", name: "Jigglypuff" },
  { key: "mewtwo", name: "Mewtwo" },
  { key: "marth", name: "Marth" },
  { key: "ike", name: "Ike" },
  { key: "roy", name: "Roy" },
  { key: "ness", name: "Ness" },
  { key: "lucas", name: "Lucas" },
  { key: "mr_game_and_watch", name: "Mr. Game & Watch" },
  { key: "snake", name: "Snake" },
  { key: "sonic", name: "Sonic" },
  { key: "knuckles", name: "Knuckles" },
  { key: "random", name: "Random" },
];

/** Most mains a profile may have: two rows of half-size stock icons (18 a row at 16 px), as the server allows. */
export const MAX_MAINS = 36;

export function characterName(key: string): string | undefined {
  return CHARACTERS.find((c) => c.key === key)?.name;
}

/** The social media a profile can link, in the order it shows them. */
export const SOCIAL_PLATFORMS: readonly { platform: SocialPlatform; name: string; placeholder: string }[] = [
  { platform: "discord", name: "Discord", placeholder: "username" },
  { platform: "twitter", name: "X (Twitter)", placeholder: "@username or x.com link" },
  { platform: "twitch", name: "Twitch", placeholder: "username or twitch.tv link" },
  { platform: "youtube", name: "YouTube", placeholder: "@handle or youtube.com link" },
  { platform: "tiktok", name: "TikTok", placeholder: "@username or tiktok.com link" },
  { platform: "instagram", name: "Instagram", placeholder: "username or instagram.com link" },
];

/**
 * Every country a profile can show, by name: flag-icons' list (ISO 3166-1 plus regions and groups such as England,
 * Catalonia, the EU) with short everyday names ("United States"). Written by tools/profiles/countries.py, with the
 * server's copy (server/crates/common/src/countries.rs).
 */
export const COUNTRIES: readonly { code: string; name: string }[] = countryList;

export function countryName(code: string): string | undefined {
  return COUNTRIES.find((c) => c.code === code)?.name;
}
