-- A Ranked game aborted for its match quality (the user's, 2026-10-29, as Slippi does since 3.6.3):
-- either player's game reports the leave 'match_quality', and the set is void (TERMINATED, "match
-- quality low"), nobody rated.
ALTER TABLE leave_reports DROP CONSTRAINT leave_reports_kind_check;
ALTER TABLE leave_reports ADD CONSTRAINT leave_reports_kind_check
    CHECK (kind IN ('left', 'opponent_left', 'match_quality'));
