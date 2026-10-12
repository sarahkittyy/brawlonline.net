-- The published ruleset a room's game was played with (docs/design/room-rules.md §1.1 E): the
-- match history shows its name, linked to /r/<code>. NULL for P+'s default, custom rules and
-- matches from before this. The name and version are as they were then; the hash is
-- Ruleset::hash (SHA-256 of its canonical JSON), what was actually played.
--
-- pp_mm's table-level INSERT on mm_matches (0005_mm_grants.sql) covers the new columns.

ALTER TABLE mm_matches
    ADD COLUMN ruleset_code    text    NULL,
    ADD COLUMN ruleset_name    text    NULL,
    ADD COLUMN ruleset_version integer NULL,
    ADD COLUMN ruleset_hash    text    NULL;
