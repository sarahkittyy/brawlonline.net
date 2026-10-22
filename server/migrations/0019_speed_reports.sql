-- How fast players' PCs run the game (docs/design/speed-check.md, 2026-10-22). Log only for now:
-- mm records, nothing is refused; `pp-admin speed` shows who the rule (common::speed) would lock
-- out of Ranked.

-- Each player's own game's word on its speed over one online game (`spectate-end`'s `perf`).
CREATE TABLE speed_reports (
    id          bigserial   PRIMARY KEY,
    uid         uuid        NOT NULL,
    match_id    text        NOT NULL,
    mode        text        NOT NULL,
    game_uid    text        NOT NULL,
    set_game    integer,
    end_method  text        NOT NULL,
    frames      integer     NOT NULL,
    fps         real        NOT NULL,
    -- shares of the game's frames, 0-1
    own_late    real        NOT NULL,
    followed    real        NOT NULL,
    peer_fps    real,
    slow        boolean     NOT NULL,
    peer_slow   boolean     NOT NULL,
    app_version text        NOT NULL,
    platform    text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    UNIQUE (game_uid, uid)
);
CREATE INDEX speed_reports_uid_idx ON speed_reports (uid, created_at);
CREATE INDEX speed_reports_created_idx ON speed_reports (created_at);

-- The waiting room's speed check: the frame rate of each 300-frame window while an Unranked or
-- Ranked search waited there.
CREATE TABLE speed_checks (
    id          bigserial   PRIMARY KEY,
    uid         uuid        NOT NULL,
    mode        text        NOT NULL,
    windows     real[]      NOT NULL,
    gaps        integer     NOT NULL,
    backend     text        NOT NULL,
    app_version text        NOT NULL,
    platform    text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX speed_checks_uid_idx ON speed_checks (uid, created_at);
CREATE INDEX speed_checks_created_idx ON speed_checks (created_at);

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        -- ON CONFLICT (game_uid, uid) DO NOTHING reads the conflict columns.
        GRANT INSERT, SELECT (game_uid, uid) ON speed_reports TO pp_mm;
        GRANT USAGE ON SEQUENCE speed_reports_id_seq TO pp_mm;
        GRANT INSERT ON speed_checks TO pp_mm;
        GRANT USAGE ON SEQUENCE speed_checks_id_seq TO pp_mm;
    END IF;
END
$$;
