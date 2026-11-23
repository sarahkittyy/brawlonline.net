import type { Ruleset } from "@common/rulesets";
import { RULESET_FORMAT } from "@common/rulesets";

import type { PublicRoom } from "@/services/backend/types";

/**
 * A room's custom rules as a ruleset, to show (the room list's ruleset with its match rules); null for P+ default, a
 * published ruleset (looked up by its code) or an older server's room, which sends no rules.
 */
export function roomCustomRuleset(room: PublicRoom): Ruleset | null {
  const r = room.ruleset;
  if (!r || r.code || !r.rules) {
    return null;
  }
  return {
    format: RULESET_FORMAT,
    name: r.name,
    version: 0,
    by: "",
    rules: r.rules,
    stages: r.stages ?? { starters: [], counterpicks: [] },
    bestOf: r.bestOf,
    stageChoice: r.stageChoice,
    alternateStages: r.alternateStages ?? false,
    strikes: r.strikes ?? [],
    bans: r.bans ?? 0,
    dsr: r.dsr ?? false,
  };
}
