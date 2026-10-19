-- Games played with each published ruleset (the user's, 2026-10-19), for the public list's order
-- (most played first) and its "played N times". One row per published version, 0 when published;
-- a version counts only its own games (a new version starts at 0: "it doesn't deserve the
-- plays"). mm adds one per game played in a room with that ruleset unchanged: each counted game of
-- a set, and a game without a set that lasted 30 s (docs/rulesets-protocol.md). A room whose
-- rules were edited plays Custom and counts nowhere.
CREATE TABLE ruleset_plays (
    code   text   PRIMARY KEY REFERENCES rulesets (code) ON DELETE CASCADE,
    games  bigint NOT NULL DEFAULT 0 CHECK (games >= 0)
);

-- Seeded once, here, with the estimate the match records give (the user's, 2026-10-19: "seed prod
-- ONCE EVER with our current estimates"): every room match recorded with the ruleset since
-- 0009_match_rulesets. A set was one match, so it counts as one game; a quit counts too.
INSERT INTO ruleset_plays (code, games)
SELECT r.code, COALESCE(m.n, 0)
  FROM rulesets r
  LEFT JOIN (SELECT ruleset_code, count(*) AS n FROM mm_matches WHERE ruleset_code IS NOT NULL
              GROUP BY ruleset_code) m ON m.ruleset_code = r.code;

-- pp-admin ruleset delete / rename (the user's, 2026-10-19): only an operator's transaction that
-- sets pp.ruleset_admin = 'on' (SET LOCAL) may delete a published ruleset or change its name, json
-- and hash; everything else stays as 0008_rulesets made it.
CREATE OR REPLACE FUNCTION rulesets_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF current_setting('pp.ruleset_admin', true) = 'on' THEN
        IF TG_OP = 'DELETE' THEN
            RETURN OLD;
        END IF;
        RETURN NEW;
    END IF;
    IF TG_OP = 'DELETE' THEN
        RAISE EXCEPTION 'published rulesets are never deleted (%)', OLD.code;
    END IF;
    IF NEW.code IS DISTINCT FROM OLD.code OR NEW.lineage IS DISTINCT FROM OLD.lineage
        OR NEW.version IS DISTINCT FROM OLD.version OR NEW.name IS DISTINCT FROM OLD.name
        OR NEW.publisher_name IS DISTINCT FROM OLD.publisher_name OR NEW.json IS DISTINCT FROM OLD.json
        OR NEW.hash IS DISTINCT FROM OLD.hash OR NEW.created_at IS DISTINCT FROM OLD.created_at
        OR (NEW.owner IS DISTINCT FROM OLD.owner AND NEW.owner IS NOT NULL) THEN
        RAISE EXCEPTION 'published rulesets never change (%); only hidden can', OLD.code;
    END IF;
    RETURN NEW;
END
$$;

-- mm reads the count with a ruleset and adds to it (UPDATE ... WHERE code = $1 needs SELECT on code).
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT, UPDATE (games) ON ruleset_plays TO pp_mm;
    END IF;
END
$$;
