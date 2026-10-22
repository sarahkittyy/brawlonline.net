-- mm keeps its matches in memory only, so a restart (every deploy) forgot the sets under way:
-- their games' spectate offers were refused for the rest of the set ("That match isn't yours to
-- stream", bug report #80, 2026-10-21) and a Ranked set's presence was no longer recorded. mm now
-- reads the last 6 hours of matches back when it starts (common::db::recent_matches), which needs
-- these columns.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT (match_id, mode, created_at, players, is_host, room_code) ON mm_matches TO pp_mm;
    END IF;
END
$$;
