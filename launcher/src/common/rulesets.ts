/**
 * Rulesets (docs/rulesets-protocol.md): the TypeScript side of `common::rules::Ruleset`
 * (server/crates/common/src/rules.rs). The same JSON, defaults and checks, so the Rulesets page
 * can say what is wrong before publishing; the server checks again and has the last word.
 */

import { stageInfo } from "./stages";

export type RuleStyle = "time" | "stock" | "coin";
export type RulePause = "normal" | "hold";
export type StageChoice = "loser-picks" | "strike" | "random";

/** `common::rules::MatchRules`. */
export type MatchRules = {
  style: RuleStyle;
  /** 1-99. */
  stocks: number;
  /** 0-99 (0: no limit) for stock; 1-99 for time and coin. */
  timeMinutes: number;
  /** 0.5-2.0 in steps of 0.1. */
  damageRatio: number;
  teamAttack: boolean;
  pause: RulePause;
  /** 0 none, 1 low, 2 medium, 3 high. */
  itemFrequency: number;
  /** The item switch screen's 8 bytes. */
  itemSwitch: number[];
  hazards: boolean;
  autoLCancel: boolean;
  inputAssist: boolean;
};

export type StageLists = {
  /** Game 1 of a set (and every `strike` game) strikes from these. */
  starters: number[];
  /** The extra stages of games 2+ (none of the starters). */
  counterpicks: number[];
};

/** `common::rules::Ruleset`. */
export type Ruleset = {
  format: number;
  name: string;
  /** Assigned when published; 0 in drafts and files. */
  version: number;
  /** The publisher, filled by the server. */
  by: string;
  rules: MatchRules;
  stages: StageLists;
  /**
   * 0 (no set), 3, 5 or 7. A ruleset is for any room: a set (and striking) applies when the room makes two sides
   * (1v1, or two teams of two); otherwise the room plays single games on a random stage from the lists.
   */
  bestOf: number;
  stageChoice: StageChoice;
  strikes: number[];
  bans: number;
  dsr: boolean;
};

export const RULESET_FORMAT = 1;
export const MAX_RULESET_NAME_LEN = 32;
export const MAX_BANS = 3;
export const BEST_OF_CHOICES = [0, 3, 5, 7] as const;

/** P+ v3.2's item switch after a fresh boot. */
export const PPLUS_ITEM_SWITCH = [0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

/** P+ v3.2's legal stages (srStageKind) with their names, in the order the editor lists them. */
export const PPLUS_LEGAL_STAGES: readonly { id: number; name: string }[] = [
  { id: 0x01, name: "Battlefield" },
  { id: 0x02, name: "Final Destination" },
  { id: 0x21, name: "Smashville" },
  { id: 0x2d, name: "Dream Land" },
  { id: 0x2e, name: "Pokémon Stadium 2" },
  { id: 0x1f, name: "Fountain of Dreams" },
  { id: 0x23, name: "Green Hill Zone" },
  { id: 0x0d, name: "Yoshi's Island" },
  { id: 0x03, name: "Delfino's Secret" },
  { id: 0x0c, name: "Frigate Husk" },
  { id: 0x04, name: "Luigi's Mansion" },
  { id: 0x05, name: "Metal Cavern" },
  { id: 0x06, name: "Bowser's Castle" },
  { id: 0x09, name: "Temple of Time" },
  { id: 0x1c, name: "Wario Land" },
];

/** Ranked's starters: the default ruleset's. */
export const PPLUS_STARTERS = [0x01, 0x02, 0x21, 0x2d, 0x2e];

export function stageName(id: number): string {
  return stageInfo(id)?.name ?? `Stage ${id}`;
}

export function defaultMatchRules(): MatchRules {
  return {
    style: "stock",
    stocks: 4,
    timeMinutes: 8,
    damageRatio: 1.0,
    teamAttack: true,
    pause: "normal",
    itemFrequency: 0,
    itemSwitch: [...PPLUS_ITEM_SWITCH],
    // Off by default (the user's decision, 2026-10-12), as the server's MatchRules::default.
    hazards: false,
    autoLCancel: false,
    inputAssist: false,
  };
}

/** P+ default (`Ruleset::default()`): what a room plays without a ruleset. */
export function defaultRuleset(): Ruleset {
  return {
    format: RULESET_FORMAT,
    name: "P+ default",
    version: 0,
    by: "",
    rules: defaultMatchRules(),
    stages: {
      starters: [...PPLUS_STARTERS],
      counterpicks: PPLUS_LEGAL_STAGES.map((s) => s.id)
        .filter((id) => !PPLUS_STARTERS.includes(id))
        .sort((a, b) => a - b),
    },
    bestOf: 0,
    stageChoice: "loser-picks",
    strikes: [],
    bans: 0,
    dsr: false,
  };
}

/** Whether the ruleset strikes (game 1 of a set, or every game with `strike`), in rooms with two sides. */
export function strikesUsed(r: Ruleset): boolean {
  return r.bestOf > 0 || r.stageChoice === "strike";
}

/**
 * The usual strike pattern for a starter count: 1, then 2s, then what is left (5: 1-2-1,
 * 7: 1-2-2-1, 3: 1-1). It leaves exactly one stage.
 */
export function suggestStrikes(starters: number): number[] {
  let left = starters - 1;
  if (left <= 0) {
    return [];
  }
  const pattern = [1];
  left -= 1;
  while (left >= 2) {
    pattern.push(2);
    left -= 2;
  }
  if (left > 0) {
    pattern.push(left);
  }
  return pattern;
}

/** `Ruleset::normalize`: clears what the set format leaves unused. Returns a new ruleset. */
export function normalizeRuleset(r: Ruleset): Ruleset {
  const n: Ruleset = { ...r, name: r.name.trim(), strikes: [...r.strikes] };
  if (n.bestOf > 0) {
    n.stageChoice = "loser-picks";
  } else {
    n.bans = 0;
    n.dsr = false;
  }
  if (!strikesUsed(n)) {
    n.strikes = [];
  }
  return n;
}

/** A display name's characters (common::codes::is_name_char): printable ASCII, Hiragana, Katakana. */
function isNameChar(c: string): boolean {
  const code = c.codePointAt(0) ?? 0;
  const ascii = code >= 0x20 && code <= 0x7e && c !== "\\" && c !== "`";
  return ascii || (code >= 0x3041 && code <= 0x3093) || (code >= 0x30a1 && code <= 0x30f3);
}

/** `MatchRules::validate`. */
export function validateMatchRules(m: MatchRules): string | null {
  if (!Number.isInteger(m.stocks) || m.stocks < 1 || m.stocks > 99) {
    return `stocks ${m.stocks} is not 1-99`;
  }
  const minTime = m.style === "stock" ? 0 : 1;
  if (!Number.isInteger(m.timeMinutes) || m.timeMinutes < minTime || m.timeMinutes > 99) {
    return `timeMinutes ${m.timeMinutes} is not ${minTime}-99`;
  }
  const tenths = m.damageRatio * 10;
  if (
    !Number.isFinite(m.damageRatio) ||
    Math.round(tenths) < 5 ||
    Math.round(tenths) > 20 ||
    Math.abs(tenths - Math.round(tenths)) > 1e-6
  ) {
    return `damageRatio ${m.damageRatio} is not 0.5-2.0 in steps of 0.1`;
  }
  if (!Number.isInteger(m.itemFrequency) || m.itemFrequency < 0 || m.itemFrequency > 3) {
    return `itemFrequency ${m.itemFrequency} is not 0-3`;
  }
  return null;
}

/**
 * `Ruleset::validate`, for a normalized ruleset: null when it can be published, else why not (the
 * server's wording).
 */
export function validateRuleset(r: Ruleset): string | null {
  if (r.format !== RULESET_FORMAT) {
    return r.format > RULESET_FORMAT
      ? "This ruleset was made by a newer version. Update to use it."
      : `format ${r.format} is not a ruleset format`;
  }
  const chars = Array.from(r.name);
  if (chars.length === 0 || r.name.trim() !== r.name) {
    return "The name can't be empty or start or end with a space";
  }
  if (chars.length > MAX_RULESET_NAME_LEN) {
    return `The name is longer than ${MAX_RULESET_NAME_LEN} characters`;
  }
  if (!chars.every(isNameChar)) {
    return "The name can only have letters, numbers, Hiragana, Katakana and punctuation";
  }
  const rules = validateMatchRules(r.rules);
  if (rules) {
    return rules;
  }
  if (r.stages.starters.length === 0) {
    return "There are no starter stages";
  }
  const seen = new Set<number>();
  for (const s of [...r.stages.starters, ...r.stages.counterpicks]) {
    if (!Number.isInteger(s) || s < 1 || s > 0xff) {
      return `Stage id ${s} is out of range`;
    }
    if (seen.has(s)) {
      return `Stage id ${s} is listed twice`;
    }
    seen.add(s);
  }
  if (!(BEST_OF_CHOICES as readonly number[]).includes(r.bestOf)) {
    return `bestOf ${r.bestOf} is not 0 (no set), 3, 5 or 7`;
  }
  if (r.bestOf > 0 && r.stageChoice !== "loser-picks") {
    return "The stage choice is for rulesets without a set";
  }
  if (strikesUsed(r)) {
    const left = r.stages.starters.length - 1;
    if (r.strikes.some((n) => !Number.isInteger(n) || n < 1)) {
      return "A strike turn strikes at least one stage";
    }
    const total = r.strikes.reduce((a, b) => a + b, 0);
    if (total !== left) {
      return `The strike pattern strikes ${total} of ${r.stages.starters.length} starters; it has to leave exactly one (${left} strikes)`;
    }
  } else if (r.strikes.length > 0) {
    return "This ruleset doesn't strike, so it has no strike pattern";
  }
  if (!Number.isInteger(r.bans) || r.bans < 0 || r.bans > MAX_BANS) {
    return `bans ${r.bans} is not 0-${MAX_BANS}`;
  }
  if (r.bestOf === 0 && (r.bans > 0 || r.dsr)) {
    return "Bans and DSR are for sets";
  }
  if (r.bans >= seen.size) {
    return "The winner's bans would leave no stage to pick";
  }
  return null;
}

/** Why the ruleset can't be published as it is (after normalizing), or null. */
export function rulesetProblem(r: Ruleset): string | null {
  return validateRuleset(normalizeRuleset(r));
}

/** Normalizes a ruleset code typed anywhere (case, full width, spaces); null unless it is one. */
export function parseRulesetCode(input: string): string | null {
  const code = input.normalize("NFKC").trim().toUpperCase();
  return /^[BCDFGHJKLMNPQRSTVWXZ]{6}$/.test(code) ? code : null;
}

const RULESET_KEYS = Object.keys(defaultRuleset());
/** Keys older rulesets had that are dropped on import: `players` (rulesets were for one player count until 2026-10-11). */
const LEGACY_RULESET_KEYS = ["players"];
const RULES_KEYS = Object.keys(defaultMatchRules());

/**
 * A ruleset from an imported JSON file (the API's format, as Export writes it): missing fields are
 * the defaults; unknown fields, a newer format or values of the wrong type are refused. `version`
 * and `by` are dropped (a published ruleset's file imports as a new draft). Throws with the reason.
 */
export function rulesetFromJson(text: string): Ruleset {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    throw new Error("This file isn't a ruleset (not JSON).");
  }
  // A whole API answer (`{code, ruleset, ...}`) carries the ruleset inside.
  const obj = isObject(parsed) && isObject(parsed.ruleset) && "code" in parsed ? parsed.ruleset : parsed;
  if (!isObject(obj)) {
    throw new Error("This file isn't a ruleset.");
  }
  const unknown = Object.keys(obj).filter((k) => !RULESET_KEYS.includes(k) && !LEGACY_RULESET_KEYS.includes(k));
  if (unknown.length > 0) {
    throw new Error(`This ruleset has fields this launcher doesn't know (${unknown.join(", ")}). Update to use it.`);
  }
  const base = defaultRuleset();
  const rules = obj.rules ?? {};
  const stages = obj.stages ?? {};
  if (!isObject(rules) || !isObject(stages)) {
    throw new Error("This file isn't a ruleset.");
  }
  const unknownRules = Object.keys(rules).filter((k) => !RULES_KEYS.includes(k));
  if (unknownRules.length > 0) {
    throw new Error(
      `This ruleset has rules this launcher doesn't know (${unknownRules.join(", ")}). Update to use it.`,
    );
  }
  const known = Object.fromEntries(Object.entries(obj).filter(([k]) => RULESET_KEYS.includes(k)));
  const r: Ruleset = {
    ...base,
    ...(known as Partial<Ruleset>),
    version: 0,
    by: "",
    rules: { ...base.rules, ...(rules as Partial<MatchRules>) },
    stages: { ...base.stages, ...(stages as Partial<StageLists>) },
  };
  const typeError = checkTypes(r);
  if (typeError) {
    throw new Error(`This ruleset has a bad value: ${typeError}.`);
  }
  if (r.format > RULESET_FORMAT) {
    throw new Error("This ruleset was made by a newer version. Update to use it.");
  }
  return r;
}

/** The JSON Export writes: the ruleset without `by` (the server fills it), indented. */
export function rulesetToJson(r: Ruleset): string {
  const { by: _by, ...rest } = r;
  return JSON.stringify(rest, null, 2) + "\n";
}

function isObject(v: unknown): v is Record<string, any> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

function checkTypes(r: Ruleset): string | null {
  const isInt = (v: unknown) => typeof v === "number" && Number.isInteger(v);
  const isIntArray = (v: unknown) => Array.isArray(v) && v.every(isInt);
  if (!isInt(r.format)) {
    return "format";
  }
  if (typeof r.name !== "string") {
    return "name";
  }
  if (!["loser-picks", "strike", "random"].includes(r.stageChoice)) {
    return "stageChoice";
  }
  if (!isInt(r.bestOf) || !isInt(r.bans) || typeof r.dsr !== "boolean") {
    return "the set";
  }
  if (!isIntArray(r.strikes) || !isIntArray(r.stages.starters) || !isIntArray(r.stages.counterpicks)) {
    return "a list";
  }
  const m = r.rules;
  if (!["time", "stock", "coin"].includes(m.style) || !["normal", "hold"].includes(m.pause)) {
    return "a rule";
  }
  if (![m.stocks, m.timeMinutes, m.itemFrequency].every(isInt) || typeof m.damageRatio !== "number") {
    return "a rule";
  }
  if (![m.teamAttack, m.hazards, m.autoLCancel, m.inputAssist].every((b) => typeof b === "boolean")) {
    return "a rule";
  }
  if (!isIntArray(m.itemSwitch) || m.itemSwitch.length !== 8 || m.itemSwitch.some((b) => b < 0 || b > 255)) {
    return "itemSwitch";
  }
  return null;
}
