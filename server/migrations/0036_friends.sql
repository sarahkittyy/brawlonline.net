-- Friends (docs/design/friends.md, Sarah's board 2026-11-14) and the ignore list on the account.
--
-- A request waits until the other player accepts it or the sender cancels it; it never expires.
-- Declining keeps the row (declined = true) so the sender sees nothing change and sending again
-- does not ask the other player again. Two players asking each other become friends at once.
CREATE TABLE friend_requests (
    from_uid   uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    to_uid     uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    created_at timestamptz NOT NULL DEFAULT now(),
    declined   boolean     NOT NULL DEFAULT false,
    PRIMARY KEY (from_uid, to_uid),
    CHECK (from_uid <> to_uid)
);
CREATE INDEX friend_requests_to ON friend_requests (to_uid);

-- One row per pair, the smaller uid first.
CREATE TABLE friendships (
    lo    uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    hi    uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    since timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (lo, hi),
    CHECK (lo < hi)
);
CREATE INDEX friendships_hi ON friendships (hi);

-- Who each player ignores (the launcher's and the game's chat). Until 2026-11-14 the list lived
-- only in each install's <User>/Online/chat-hidden.json; the launcher uploads that file once per
-- account (users.ignores_imported) and keeps writing it from this table for the game.
CREATE TABLE ignores (
    uid        uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    ignored    uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (uid, ignored),
    CHECK (uid <> ignored)
);

ALTER TABLE users ADD COLUMN ignores_imported boolean NOT NULL DEFAULT false;
