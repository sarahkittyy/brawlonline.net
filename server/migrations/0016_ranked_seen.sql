-- What mm sees of a Ranked set's games (2026-10-21), for checking a game only one player
-- reported (common::ranked::resolve). Ranked games are always streamed to mm (the other player's
-- game a witness), and every player's game tells mm how each game ended (`spectate-end`).

-- Whether each player's game is connected to mm: written when the set is made and whenever a
-- player's online connection opens or closes while the set is recent.
CREATE TABLE ranked_presence (
    match_id   text        NOT NULL,
    uid        uuid        NOT NULL,
    online     boolean     NOT NULL,
    changed_at timestamptz NOT NULL,
    PRIMARY KEY (match_id, uid)
);

-- How a set's game ended as one player's own game told mm: only games that ended by a game set or
-- a quit. game_index is the result report's (a desynced game played again keeps its number).
CREATE TABLE ranked_game_results (
    match_id   text        NOT NULL,
    game_index integer     NOT NULL,
    uid        uuid        NOT NULL,
    winner     uuid,
    end_method text        NOT NULL,
    created_at timestamptz NOT NULL,
    PRIMARY KEY (match_id, game_index, uid)
);
