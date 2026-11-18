-- mm reads its recent matches back when it starts (0020_mm_reads_matches.sql) and now reads each
-- one's build of the game with them (0038_match_game.sql's `game`): a mod's match read back is
-- never listed for spectators, as when it was made (docs/design/pplus-mods.md §3). pp_mm reads
-- mm_matches by column, so the new one needs its grant; without it the whole read-back fails
-- ("permission denied for table mm_matches") and mm starts having forgotten the sets under way.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT (game) ON mm_matches TO pp_mm;
    END IF;
END
$$;
