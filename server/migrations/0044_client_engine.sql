-- Which engine ran each player's game (docs/design/brawl-online-core.md §10, "Which engine",
-- 2026-11-19): our Dolphin or EmissaryRecomp, both running the shared online core. Games say it in
-- mm's `hello` and tickets and in their game and leave reports (`engine`, `engineVersion`;
-- common::client_engine), so desync and speed numbers can be split by engine (`pp-admin speed`).
--
-- Every row before this was our Dolphin's (the recomp never played on these servers): 'dolphin',
-- and no version (a Dolphin's is its appVersion). Numbered past the tournaments branch's 0043.
--
-- pp_mm's table grants on speed_reports and speed_checks (0019) cover the new columns.

ALTER TABLE speed_reports
    ADD COLUMN engine         text NOT NULL DEFAULT 'dolphin',
    ADD COLUMN engine_version text NOT NULL DEFAULT '';

ALTER TABLE speed_checks
    ADD COLUMN engine         text NOT NULL DEFAULT 'dolphin',
    ADD COLUMN engine_version text NOT NULL DEFAULT '';

-- The reporter's engine (each player reports a game from their own game).
ALTER TABLE game_reports
    ADD COLUMN engine         text NOT NULL DEFAULT 'dolphin',
    ADD COLUMN engine_version text NOT NULL DEFAULT '';

ALTER TABLE leave_reports
    ADD COLUMN engine         text NOT NULL DEFAULT 'dolphin',
    ADD COLUMN engine_version text NOT NULL DEFAULT '';
