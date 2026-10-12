-- The public list of rulesets (GET /v1/rulesets, docs/rulesets-protocol.md §3): every ruleset's
-- newest version that is not hidden, newest first. The first index picks each lineage's newest
-- visible version (DISTINCT ON (lineage) ... ORDER BY lineage, version DESC WHERE NOT hidden), the
-- second serves the order and the keyset cursor (created_at, code). The search (ILIKE on the name,
-- the publisher and the code) scans what is left; the list is small.

CREATE INDEX rulesets_visible_lineage_idx ON rulesets (lineage, version DESC) WHERE NOT hidden;
CREATE INDEX rulesets_created_idx ON rulesets (created_at DESC, code DESC);
