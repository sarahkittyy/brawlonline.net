-- mm's database role. mm faces the internet (UDP 43113), so pp_mm gets only what mm's queries
-- need (server/crates/common/src/db.rs: fetch_mm_user, insert_match), never password hashes,
-- emails or tokens. A migration that adds something mm reads or writes grants it here too.
-- Release 20261009-890ecee537 shipped without the ratings grant and every search answered
-- "Matchmaking unavailable" until it was granted by hand.
--
-- Only where the role exists: production creates pp_mm before accounts first starts
-- (deploy/README.md); dev databases have no pp_mm (mm connects as the owner there).

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT USAGE ON SCHEMA public TO pp_mm;
        GRANT SELECT (uid, display_name, connect_code, play_key_version, banned_until, email_verified_at)
            ON users TO pp_mm;
        GRANT SELECT (uid, rating, sets_played) ON ratings TO pp_mm;
        -- INSERT ... ON CONFLICT (match_id) DO NOTHING needs SELECT on the conflict column.
        GRANT INSERT, SELECT (match_id) ON mm_matches TO pp_mm;
    END IF;
END
$$;
