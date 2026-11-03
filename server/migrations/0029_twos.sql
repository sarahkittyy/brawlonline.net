-- The 2v2 queue (docs/design/2v2.md, Sarah's board 2026-11-02): mode 5 matches of four players on
-- two teams, and their hidden rating (common::twos), read only by matchmaking, never shown.

-- Each player's pair in a 2v2 match, in `players` order: 0 searched alone, else the same number for
-- the two who queued together (one team in every game). NULL for every other mode.
ALTER TABLE mm_matches ADD COLUMN pairs smallint[];

-- A player's 2v2 rating. No row: no rated 2v2 game yet (mm uses the Unranked rating).
CREATE TABLE twos_ratings (
    uid         uuid             PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    rating      double precision NOT NULL,
    games       integer          NOT NULL DEFAULT 0,
    updated_at  timestamptz      NOT NULL DEFAULT now()
);

-- Every rated 2v2 game, once (the first report with a winner rates it): who played on which team,
-- who left, and every player's rating before and after.
CREATE TABLE twos_rated_games (
    match_id     text        NOT NULL REFERENCES mm_matches (match_id) ON DELETE CASCADE,
    game_index   integer     NOT NULL,
    winner_team  smallint    NOT NULL,
    players      jsonb       NOT NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (match_id, game_index)
);

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        -- common::db's account query.
        GRANT SELECT (uid, rating) ON twos_ratings TO pp_mm;
    END IF;
END
$$;
