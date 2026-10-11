-- Published rulesets, drafts, the remembered ruleset and recently used ones
-- (docs/design/room-rules.md §4, docs/rulesets-protocol.md).
--
-- A published version never changes and is never deleted (a tournament's link must keep
-- working): a trigger refuses every UPDATE but `hidden` (and the owner going to NULL when an
-- account is deleted) and every DELETE. Editing a ruleset publishes a new version, with a new
-- code, in the same `lineage`.

CREATE TABLE rulesets (
    -- 6 letters of the room codes' 20, server-assigned at random.
    code           text        PRIMARY KEY CHECK (code ~ '^[BCDFGHJKLMNPQRSTVWXZ]{6}$'),
    -- NULL once the publisher's account is deleted; the ruleset stays.
    owner          uuid        NULL REFERENCES users (uid) ON DELETE SET NULL,
    -- Every version of one ruleset shares it.
    lineage        uuid        NOT NULL,
    version        integer     NOT NULL CHECK (version >= 1),
    name           text        NOT NULL,
    -- The publisher's display name when published: shown once the account is gone.
    publisher_name text        NOT NULL,
    -- common::rules::Ruleset as canonical JSON (Ruleset::canonical_json, without `by`), exactly
    -- the bytes `hash` is taken of. text, not jsonb, so the bytes stay as they were hashed.
    json           text        NOT NULL,
    -- SHA-256 of `json`, lowercase hex.
    hash           text        NOT NULL CHECK (hash ~ '^[0-9a-f]{64}$'),
    -- Hidden from the owner's list (and from "newer version" links); the code keeps working.
    hidden         boolean     NOT NULL DEFAULT false,
    created_at     timestamptz NOT NULL DEFAULT now(),
    UNIQUE (lineage, version)
);

CREATE INDEX rulesets_owner_idx ON rulesets (owner, created_at DESC);

CREATE FUNCTION rulesets_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
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

CREATE TRIGGER rulesets_immutable BEFORE UPDATE OR DELETE ON rulesets
    FOR EACH ROW EXECUTE FUNCTION rulesets_immutable();

-- The owner's unpublished rulesets: editable, usable by nobody until published.
CREATE TABLE ruleset_drafts (
    id          uuid        PRIMARY KEY,
    owner       uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    -- The published version this draft edits: publishing it makes the next version of that
    -- ruleset if the owner published it, else a new ruleset.
    based_on    text        NULL REFERENCES rulesets (code),
    name        text        NOT NULL,
    -- common::rules::Ruleset; any values (a draft may not validate yet).
    json        jsonb       NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    updated_at  timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX ruleset_drafts_owner_idx ON ruleset_drafts (owner, updated_at DESC);

-- One remembered ruleset per account, shared by the launcher's box and in-game Create Room
-- (decision 11). No row: P+ default.
CREATE TABLE remembered_rulesets (
    uid         uuid        PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    code        text        NOT NULL REFERENCES rulesets (code),
    updated_at  timestamptz NOT NULL DEFAULT now()
);

-- Recently used rulesets (made the remembered one), for the in-game Ruleset line. The newest
-- 10 per account are kept (common::db::RECENT_RULESETS_KEPT).
CREATE TABLE ruleset_recent (
    uid         uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    code        text        NOT NULL REFERENCES rulesets (code),
    used_at     timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (uid, code)
);

-- mm (in-game Create Room with the remembered ruleset, the RULES screen's Ruleset line, Enter
-- code) reads rulesets and reads and writes the remembered and recent ones; never drafts.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT ON rulesets TO pp_mm;
        GRANT SELECT, INSERT, UPDATE, DELETE ON remembered_rulesets TO pp_mm;
        GRANT SELECT, INSERT, UPDATE, DELETE ON ruleset_recent TO pp_mm;
    END IF;
END
$$;
