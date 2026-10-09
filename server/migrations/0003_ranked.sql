-- Ranked: Elo ratings, the two clients' game reports and leave reports, rating history.
-- Rules: server/crates/common/src/ranked.rs. A ranked set is an mm_matches row with mode 0;
-- its status goes from ASSIGNED to COMPLETE, ABANDONED, TERMINATED, ERROR or ORPHANED.

-- A player's rating. No row: never played a ranked set (DEFAULT_RATING, 0 sets).
CREATE TABLE ratings (
    uid          uuid             PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    rating       double precision NOT NULL,
    sets_played  integer          NOT NULL DEFAULT 0,
    wins         integer          NOT NULL DEFAULT 0,
    losses       integer          NOT NULL DEFAULT 0,
    updated_at   timestamptz      NOT NULL DEFAULT now()
);
CREATE INDEX ratings_rating_idx ON ratings (rating DESC);

-- One client's report of one game of a ranked set. winner NULL: a draw.
CREATE TABLE game_reports (
    match_id         text        NOT NULL REFERENCES mm_matches (match_id) ON DELETE CASCADE,
    game_index       integer     NOT NULL CHECK (game_index >= 1),
    reporter         uuid        NOT NULL,
    winner           uuid        NULL,
    stage_id         integer     NULL,
    duration_frames  integer     NULL,
    -- The rest of the report (characters, stocks, damage), kept for review.
    detail           jsonb       NOT NULL DEFAULT '{}'::jsonb,
    created_at       timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (match_id, game_index, reporter)
);

-- A client's report that the set ended early: 'left' (the reporter left) or 'opponent_left'.
CREATE TABLE leave_reports (
    match_id    text        NOT NULL REFERENCES mm_matches (match_id) ON DELETE CASCADE,
    reporter    uuid        NOT NULL,
    kind        text        NOT NULL CHECK (kind IN ('left', 'opponent_left')),
    created_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (match_id, reporter, kind)
);

-- How a ranked set ended.
ALTER TABLE mm_matches
    ADD COLUMN winner      uuid        NULL,
    ADD COLUMN ended_at    timestamptz NULL,
    ADD COLUMN end_reason  text        NULL;
CREATE INDEX mm_matches_open_ranked_idx ON mm_matches (created_at) WHERE mode = 0 AND status = 'ASSIGNED';

-- Every rating change, one row per player per set.
CREATE TABLE rating_events (
    id               bigserial        PRIMARY KEY,
    uid              uuid             NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    match_id         text             NOT NULL REFERENCES mm_matches (match_id) ON DELETE CASCADE,
    opponent         uuid             NULL,
    -- 1 set win, 0 set loss.
    score            double precision NOT NULL,
    rating_before    double precision NOT NULL,
    rating_after     double precision NOT NULL,
    opponent_rating  double precision NOT NULL,
    k                double precision NOT NULL,
    sets_before      integer          NOT NULL,
    created_at       timestamptz      NOT NULL DEFAULT now(),
    UNIQUE (match_id, uid)
);
CREATE INDEX rating_events_uid_idx ON rating_events (uid, created_at);
