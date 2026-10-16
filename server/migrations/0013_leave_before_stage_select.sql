-- A ranked "left" made before the leaver's game was sent to the set's stage select (bug report
-- #37, 2026-10-16: a search cancelled just as the match was made). With no game played the set is
-- not rated (common::ranked::resolve). Older clients never say so: false, as before.
ALTER TABLE leave_reports
    ADD COLUMN before_stage_select boolean NOT NULL DEFAULT false;
