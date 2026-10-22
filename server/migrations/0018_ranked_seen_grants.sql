-- mm writes ranked_presence and ranked_game_results (0016_ranked_seen.sql), and 0016 granted its
-- role nothing on them: in production every write failed with "permission denied" (4,834 in the
-- 24 h to 2026-10-22 00:00 UTC, both tables empty), so accounts saw no game of a set's other
-- player and a lone report counted after the grace period, as before 0016. The upserts need SELECT
-- (ON CONFLICT ... WHERE reads the row) and UPDATE.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT, INSERT, UPDATE ON ranked_presence TO pp_mm;
        GRANT SELECT, INSERT, UPDATE ON ranked_game_results TO pp_mm;
    END IF;
END
$$;
