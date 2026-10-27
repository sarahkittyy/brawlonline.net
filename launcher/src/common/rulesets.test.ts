import { describe, expect, it } from "vitest";

import type { Ruleset } from "./rulesets";
import {
  defaultRuleset,
  hazardsOn,
  normalizeRuleset,
  parseRulesetCode,
  rulesetFromJson,
  rulesetProblem,
  rulesetToJson,
  suggestStrikes,
  validateRuleset,
  withStageHazards,
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
      hazardStages: [],
      autoLCancel: false,
      inputAssist: false,
      stockControl: false,
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
      [{ ...defaultRuleset(), stageChoice: "random", alternateStages: true }, "Alternate stages"],
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
    // Alternate stages: kept with a pick (a set, the loser), cleared where nobody picks.
    expect(defaultRuleset().alternateStages).toBe(false);
    expect(normalizeRuleset(weekly({ alternateStages: true })).alternateStages).toBe(true);
    expect(
      normalizeRuleset({ ...defaultRuleset(), stageChoice: "loser-picks-list", alternateStages: true }).alternateStages,
    ).toBe(true);
    expect(normalizeRuleset({ ...n, alternateStages: true }).alternateStages).toBe(false);
    expect(rulesetProblem({ ...defaultRuleset(), stageChoice: "random", alternateStages: true })).toBeNull();
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
    // Stock control is written only when on (as the server writes it), and read back.
    expect(JSON.parse(text).rules.stockControl).toBeUndefined();
    const sc = weekly({ rules: { ...defaultRuleset().rules, stockControl: true } });
    expect(JSON.parse(rulesetToJson(sc)).rules.stockControl).toBe(true);
    expect(rulesetFromJson(rulesetToJson(sc))).toEqual(sc);
    expect(() => rulesetFromJson(`{"rules": {"stockControl": 1}}`)).toThrow("bad value");
    // Alternate stages too: written only when on, read back.
    expect("alternateStages" in JSON.parse(text)).toBe(false);
    const alts = weekly({ alternateStages: true });
    expect(JSON.parse(rulesetToJson(alts)).alternateStages).toBe(true);
    expect(rulesetFromJson(rulesetToJson(alts))).toEqual(alts);
    expect(rulesetFromJson(`{"name": "x", "alternateStages": true}`).alternateStages).toBe(true);
    expect(() => rulesetFromJson(`{"alternateStages": "yes"}`)).toThrow("bad value");
    // Per-stage hazards: written only when a stage has its own, read back.
    expect("hazardStages" in JSON.parse(text).rules).toBe(false);
    const fod = weekly({ rules: { ...defaultRuleset().rules, hazardStages: [0x1f] } });
    expect(JSON.parse(rulesetToJson(fod)).rules.hazardStages).toEqual([31]);
    expect(rulesetFromJson(rulesetToJson(fod))).toEqual(fod);
    expect(() => rulesetFromJson(`{"rules": {"hazardStages": [true]}}`)).toThrow("bad value");
  });

  it("has per-stage hazards", () => {
    const m = withStageHazards(defaultRuleset().rules, 0x1f);
    expect(m.hazardStages).toEqual([0x1f]);
    expect([hazardsOn(m, 0x1f), hazardsOn(m, 0x01)]).toEqual([true, false]);
    // The default flipped: the stage with its own stays the other way.
    expect([hazardsOn({ ...m, hazards: true }, 0x1f), hazardsOn({ ...m, hazards: true }, 0x01)]).toEqual([false, true]);
    // Kept sorted; a second click gives the stage back to the default.
    expect(withStageHazards(m, 0x01).hazardStages).toEqual([0x01, 0x1f]);
    expect(withStageHazards(m, 0x1f).hazardStages).toEqual([]);
    // As the server: at most 8, each once, in range; a ruleset keeps them sorted.
    const rules = (hazardStages: number[]) => weekly({ rules: { ...defaultRuleset().rules, hazardStages } });
    expect(rulesetProblem(rules([1, 2, 3, 4, 5, 6, 7, 8]))).toBeNull();
    expect(rulesetProblem(rules([1, 2, 3, 4, 5, 6, 7, 8, 9]))).toContain("At most 8");
    expect(validateRuleset(rules([31, 31]))).toContain("twice");
    expect(validateRuleset(rules([0]))).toContain("range");
    expect(normalizeRuleset(rules([31, 1, 31])).rules.hazardStages).toEqual([1, 31]);
  });
});
