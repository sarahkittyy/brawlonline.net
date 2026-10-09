-- The ranked leaderboard and a player's match history.

-- Leaderboard order (rating, then uid), only players with a rated set: keyset pages and the
-- position counts read this index.
CREATE INDEX ratings_leaderboard_idx ON ratings (rating DESC, uid) WHERE sets_played > 0;

-- A player's matches (GET /v1/me/matches): `players @> ARRAY[uid]`.
CREATE INDEX mm_matches_players_idx ON mm_matches USING gin (players);
