import { describe, expect, it } from "vitest";

import type { Ruleset } from "./rulesets";
import {
  defaultRuleset,
  normalizeRuleset,
  parseRulesetCode,
  rulesetFromJson,
  rulesetProblem,
  rulesetToJson,
  suggestStrikes,
  validateRuleset,
} from "./rulesets";

/** The Bo3 of the server's tests (server/crates/common/src/rules.rs `weekly`). */
const weekly = (over: Partial<Ruleset> = {}): Ruleset => ({
  ...defaultRuleset(),
  name: "NorCal Weekly",
  bestOf: 3,
  strikes: [1, 2, 1],
  bans: 2,
  dsr: true,
  ...over,
});

describe("rulesets", () => {
  it("has the server's defaults", () => {
    const r = defaultRuleset();
    expect(validateRuleset(r)).toBeNull();
    // As `Ruleset::default()` serializes (rules.rs canonical JSON test).
    expect(r.stages).toEqual({
      starters: [1, 4, 9, 33, 46],
      counterpicks: [12, 31, 35, 45],
    });
    expect(r.rules).toEqual({
      style: "stock",
      stocks: 4,
      timeMinutes: 8,
      damageRatio: 1,
      teamAttack: true,
      pause: "normal",
      itemFrequency: 0,
      itemSwitch: [0, 4, 0, 0, 0, 0, 0, 0],
      hazards: false,
      autoLCancel: false,
      inputAssist: false,
    });
    expect(validateRuleset(weekly())).toBeNull();
  });

  it("refuses what the server refuses", () => {
    const three = { starters: [1, 2, 33], counterpicks: [] };
    const bad: [Ruleset, string][] = [
      [weekly({ format: 2 }), "newer"],
      [weekly({ name: "" }), "empty"],
      [weekly({ name: " x" }), "space"],
      [weekly({ name: "x".repeat(33) }), "longer"],
      [weekly({ name: "emoji 🎮" }), "letters"],
      [weekly({ rules: { ...defaultRuleset().rules, stocks: 0 } }), "stocks"],
      [weekly({ rules: { ...defaultRuleset().rules, damageRatio: 1.05 } }), "damageRatio"],
      [weekly({ stages: { starters: [], counterpicks: [1] } }), "starter"],
      [weekly({ stages: { starters: [1], counterpicks: [1] }, strikes: [] }), "twice"],
      [weekly({ bestOf: 2 }), "bestOf"],
      [weekly({ strikes: [1, 2] }), "exactly one"],
      [weekly({ strikes: [0, 1, 2, 1] }), "at least one"],
      [weekly({ stages: three }), "exactly one"],
      [{ ...defaultRuleset(), strikes: [1, 2, 1] }, "doesn't strike"],
      [weekly({ bans: 4 }), "bans"],
      [{ ...defaultRuleset(), dsr: true }, "for sets"],
      [
        weekly({
          stages: { starters: [1, 2, 3], counterpicks: [] },
          strikes: [1, 1],
          bans: 3,
        }),
        "no stage",
      ],
    ];
    for (const [r, why] of bad) {
      expect(validateRuleset(r), why).toContain(why);
    }
    expect(validateRuleset(weekly({ stages: three, strikes: [1, 1] }))).toBeNull();
    expect(validateRuleset(weekly({ name: "スマブラ カップ" }))).toBeNull();
    // Rulesets are for any room: no player count, so a set and striking are always allowed.
    expect(validateRuleset({ ...defaultRuleset(), stageChoice: "strike", strikes: [1, 2, 1] })).toBeNull();
  });

  it("normalizes like the server", () => {
    const n = normalizeRuleset({
      ...defaultRuleset(),
      name: "  Bo1 ",
      stageChoice: "strike",
      strikes: [1, 2, 1],
      bans: 2,
      dsr: true,
    });
    expect([n.name, n.bans, n.dsr, n.strikes]).toEqual(["Bo1", 0, false, [1, 2, 1]]);
    expect(normalizeRuleset({ ...n, stageChoice: "random" }).strikes).toEqual([]);
    expect(normalizeRuleset(weekly({ stageChoice: "random" })).stageChoice).toBe("loser-picks");
    expect(rulesetProblem({ ...defaultRuleset(), bans: 2 })).toBeNull();
    expect(normalizeRuleset(weekly())).toEqual(weekly());
  });

  it("suggests strike patterns that leave one stage", () => {
    expect(suggestStrikes(5)).toEqual([1, 2, 1]);
    expect(suggestStrikes(7)).toEqual([1, 2, 2, 1]);
    expect(suggestStrikes(3)).toEqual([1, 1]);
    expect(suggestStrikes(2)).toEqual([1]);
    expect(suggestStrikes(1)).toEqual([]);
    for (let n = 1; n <= 15; n++) {
      expect(suggestStrikes(n).reduce((a, b) => a + b, 0)).toBe(n - 1);
    }
  });

  it("parses codes", () => {
    expect(parseRulesetCode(" kfqbtr ")).toBe("KFQBTR");
    expect(parseRulesetCode("ＫＦＱＢＴＲ")).toBe("KFQBTR");
    for (const bad of ["", "KFQB", "KFQBTRX", "AFQBTR", "KFQ BT"]) {
      expect(parseRulesetCode(bad)).toBeNull();
    }
  });

  it("imports and exports files", () => {
    const r = weekly({ version: 3, by: "sarah" });
    const text = rulesetToJson(r);
    expect(JSON.parse(text).by).toBeUndefined();
    expect(rulesetFromJson(text)).toEqual(weekly());
    // An API answer imports too; missing fields are the defaults.
    expect(rulesetFromJson(JSON.stringify({ code: "KFQBTR", ruleset: r, hash: "x" }))).toEqual(weekly());
    expect(rulesetFromJson(`{"name": "x", "rules": {"stocks": 3}}`)).toEqual({
      ...defaultRuleset(),
      name: "x",
      rules: { ...defaultRuleset().rules, stocks: 3 },
    });
    expect(() => rulesetFromJson("nope")).toThrow("not JSON");
    expect(() => rulesetFromJson("[1]")).toThrow("isn't a ruleset");
    expect(() => rulesetFromJson(`{"bestof": 3}`)).toThrow("doesn't know (bestof)");
    expect(() => rulesetFromJson(`{"rules": {"stonks": 3}}`)).toThrow("stonks");
    expect(() => rulesetFromJson(`{"format": 2}`)).toThrow("newer version");
    expect(() => rulesetFromJson(`{"stageChoice": "rps"}`)).toThrow("bad value");
    // An older file's player count is dropped.
    expect(rulesetFromJson(`{"name": "x", "players": "ffa"}`)).toEqual({ ...defaultRuleset(), name: "x" });
    expect("players" in rulesetFromJson(`{"players": "1v1"}`)).toBe(false);
    expect(() => rulesetFromJson(`{"rules": {"itemSwitch": [1]}}`)).toThrow("itemSwitch");
  });
});
