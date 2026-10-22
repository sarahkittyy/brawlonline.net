-- Each online game's input delay and what it cost on the player's screen (2026-10-22): the delay
-- profiles (Aggressive, Normal, Smooth; GameplaySession.h, ProfileInputDelay) were tuned from 44
-- players' bug report logs; every player's games now say it (`spectate-end`'s `perf`, common::speed
-- GameDelay). Null for games of older builds.
--
-- pp_mm's table grants (0019) cover the new columns.

ALTER TABLE speed_reports
    ADD COLUMN delay               smallint,
    -- the profile the players agreed on, and the one this player asked for
    ADD COLUMN delay_profile       text,
    ADD COLUMN delay_asked         text,
    ADD COLUMN delay_fixed         boolean,
    -- GekkoNet's ping the delay was picked from (ms; -1 none), its average and jitter over the game
    ADD COLUMN rtt_start           real,
    ADD COLUMN ping                real,
    ADD COLUMN jitter              real,
    -- shares of the game's frames 0-1 (hitches: of its presents), depth in frames, costs in ms
    ADD COLUMN rollbacks           real,
    ADD COLUMN depth               real,
    ADD COLUMN rollback_ms         real,
    ADD COLUMN late_after_rollback real,
    ADD COLUMN hitches             real,
    ADD COLUMN waits               integer,
    ADD COLUMN wait_ms             real,
    ADD COLUMN resim_ms            real;
