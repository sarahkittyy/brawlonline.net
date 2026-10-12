import { describe, expect, it } from "vitest";

import { defaultRuleset, PPLUS_LEGAL_STAGES, stageName } from "./rulesets";
import { PPLUS_STAGES, stageInfo } from "./stages";

describe("stages", () => {
  it("lists P+'s 61 stage select stages once each, with an icon each", () => {
    expect(PPLUS_STAGES).toHaveLength(61);
    expect(new Set(PPLUS_STAGES.map((s) => s.id)).size).toBe(61);
    expect(new Set(PPLUS_STAGES.map((s) => s.icon)).size).toBe(61);
  });

  it("has the legal stages on page 1 under the same names", () => {
    const page1 = PPLUS_STAGES.slice(0, 21).map((s) => s.id);
    for (const s of PPLUS_LEGAL_STAGES) {
      expect(page1).toContain(s.id);
      expect(stageInfo(s.id)?.name).toBe(s.name);
    }
    // Battlefield and Final Destination at the stage select's positions 17 and 10.
    expect(PPLUS_STAGES[17].name).toBe("Battlefield");
    expect(PPLUS_STAGES[10].name).toBe("Final Destination");
  });

  it("names every stage, and an unknown id by its number", () => {
    expect(stageName(0x2a)).toBe("Yoshi's Story");
    expect(stageName(0x4e)).toBe("Subspace");
    expect(stageName(0xfe)).toBe("Stage 254");
  });

  it("leaves every stage outside the legal list off in the default ruleset", () => {
    const r = defaultRuleset();
    const on = [...r.stages.starters, ...r.stages.counterpicks].sort((a, b) => a - b);
    expect(on).toEqual(PPLUS_LEGAL_STAGES.map((s) => s.id).sort((a, b) => a - b));
  });
});
