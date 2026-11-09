-- One row per Unranked, Ranked or 2v2 ticket mm ended (Sarah's board, 2026-11-09: `pp-admin stats`,
-- the queue counts and an estimated wait by hour of day; nothing in the launcher). Written when the
-- ticket is matched or given up (the player cancelled, closed the game, was replaced by a newer
-- search or banned). A failed P2P connect requeues with a new ticket a few seconds after its match,
-- and a 2v2 group that broke up searches again with `requeue`: `pp-admin stats` joins those to the
-- search before them.
CREATE TABLE queue_searches (
    id        bigserial   PRIMARY KEY,
    uid       uuid        NOT NULL,
    -- 'unranked', 'ranked' or 'twos'
    mode      text        NOT NULL,
    -- from the ticket to its end; started = ended_at - waited_ms
    waited_ms integer     NOT NULL,
    matched   boolean     NOT NULL,
    -- 2v2: the search started again by itself after its group broke up
    requeue   boolean     NOT NULL DEFAULT false,
    ended_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX queue_searches_ended_idx ON queue_searches (ended_at);

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT INSERT ON queue_searches TO pp_mm;
        GRANT USAGE ON SEQUENCE queue_searches_id_seq TO pp_mm;
    END IF;
END
$$;
