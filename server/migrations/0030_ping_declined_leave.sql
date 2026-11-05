-- A Ranked set that ended in its ping check, before game 1 (bug report #226, Sarah's board
-- 2026-11-05): the players' round trip was one the match quality aborts a game for, and either
-- player chose not to play it. Both games report the leave 'ping_declined', and the set is void
-- (TERMINATED, "not played: high ping"), nobody rated or at fault.
ALTER TABLE leave_reports DROP CONSTRAINT leave_reports_kind_check;
ALTER TABLE leave_reports ADD CONSTRAINT leave_reports_kind_check
    CHECK (kind IN ('left', 'opponent_left', 'match_quality', 'ping_declined'));
