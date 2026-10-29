-- A Ranked set ended on one player's own end (the user's, 2026-10-29: a player losing a set must
-- not get it voided by slowing their game down or pulling the cable; "only when we know for a
-- fact its their fault"). common::ranked::resolve decides it from:
--
-- * the games' word on an abort for match quality: `cause` 'self' (this PC missed its own frame
--   schedule), 'peer' (the other PC said it did) or 'link'; and whether the abort or the
--   "opponent left" came early in the set (`early`: the first 15 s of game 1, or after a game 1
--   that was poor from its start). NULL: a game from before this, taken as early.
-- * mm's record of trouble on a player's own connection to mm while in the set
--   (ranked_net_events): it went silent (the network went, or the game stopped running) or its
--   round trip jumped. mm is a third party: trouble only one player's connection had is theirs.
--
-- The at-fault player (mm_matches.fault) loses the set (ABANDONED, "abandoned by <uid>: match
-- quality low" or ": connection lost"), or early in the set it is void as before; either way it
-- counts toward a Ranked cooldown (3 in 24 h: 1 h), which mm reads at a Ranked search.

ALTER TABLE leave_reports
    ADD COLUMN early boolean,
    ADD COLUMN cause text CHECK (cause IN ('self', 'peer', 'link'));

CREATE TABLE ranked_net_events (
    match_id   text        NOT NULL,
    uid        uuid        NOT NULL,
    -- 'silence': nothing came from the game for a while; 'lag': its round trip to mm jumped.
    kind       text        NOT NULL CHECK (kind IN ('silence', 'lag')),
    started_at timestamptz NOT NULL,
    ended_at   timestamptz NOT NULL,
    -- 'lag': the highest round trip, and the connection's usual one before it (ms).
    peak_ms    integer,
    base_ms    integer
);
CREATE INDEX ranked_net_events_match_idx ON ranked_net_events (match_id);

ALTER TABLE mm_matches ADD COLUMN fault uuid;
CREATE INDEX mm_matches_fault_idx ON mm_matches (fault, ended_at) WHERE fault IS NOT NULL;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT INSERT ON ranked_net_events TO pp_mm;
        -- The cooldown in common::db's account query.
        GRANT SELECT (fault, ended_at) ON mm_matches TO pp_mm;
    END IF;
END
$$;
