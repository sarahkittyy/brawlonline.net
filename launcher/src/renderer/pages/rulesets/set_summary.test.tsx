import { defaultRuleset } from "@common/rulesets";
import { describe, expect, it } from "vitest";

import { setSummary } from "./rulesets_page";
import { RulesetsPageMessages } from "./rulesets_page.messages";

describe("ruleset rows", () => {
  it("summarize the set", () => {
    const r = defaultRuleset();
    expect(setSummary({ ...r, bestOf: 3, strikes: [1, 2, 1], bans: 2 })).toBe("Bo3 · strikes 1-2-1 · 2 bans");
    expect(setSummary({ ...r, bestOf: 5, strikes: [1, 2, 1], bans: 0 })).toBe("Bo5 · strikes 1-2-1");
    expect(setSummary(r)).toBe("No set · loser picks");
    expect(setSummary({ ...r, stageChoice: "loser-picks-list" })).toBe("No set · loser picks from the list");
    expect(setSummary({ ...r, stageChoice: "strike", strikes: [1, 1] })).toBe("No set · strikes 1-1");
    expect(setSummary({ ...r, stageChoice: "random" })).toBe("No set · random stage");
  });

  it("say how often the version was played", () => {
    expect(RulesetsPageMessages.played(1423)).toBe("played 1423 times");
    expect(RulesetsPageMessages.played(1)).toBe("played 1 time");
    expect(RulesetsPageMessages.played(0)).toBe("played 0 times");
  });
});
