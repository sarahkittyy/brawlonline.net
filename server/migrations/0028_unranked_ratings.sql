-- Unranked's hidden rating (common::unranked): an Elo moved after every Unranked game, read only
-- by matchmaking, which prefers close ratings in the Unranked queue (the user's, 2026-10-31).
-- Players never see it. accounts rates a game on its first report; at its first start with this
-- table empty it fills both tables by replaying every Unranked game reported so far
-- (accounts::unranked::backfill; `pp-admin unranked rebuild` does it again on demand).

-- A player's Unranked rating. No row: no rated Unranked game yet (mm uses the Ranked rating).
CREATE TABLE unranked_ratings (
    uid         uuid             PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    rating      double precision NOT NULL,
    games       integer          NOT NULL DEFAULT 0,
    updated_at  timestamptz      NOT NULL DEFAULT now()
);

-- Every rated Unranked game, once (the first report rates it), with both changes.
CREATE TABLE unranked_rated_games (
    match_id       text             NOT NULL REFERENCES mm_matches (match_id) ON DELETE CASCADE,
    game_index     integer          NOT NULL,
    winner         uuid             NOT NULL,
    loser          uuid             NOT NULL,
    winner_before  double precision NOT NULL,
    loser_before   double precision NOT NULL,
    winner_after   double precision NOT NULL,
    loser_after    double precision NOT NULL,
    created_at     timestamptz      NOT NULL DEFAULT now(),
    PRIMARY KEY (match_id, game_index)
);

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        -- common::db's account query.
        GRANT SELECT (uid, rating) ON unranked_ratings TO pp_mm;
    END IF;
END
$$;
