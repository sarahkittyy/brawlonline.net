-- The build of the game a match is played on (docs/design/pplus-mods.md §3): mm's id of a mod of
-- P+ (`cobalt-1.34`, `pex-1.8`; mm's config/games.json), '' for P+ itself and every match before
-- this. Two builds never share a match. The match history carries it, so a mod's characters and
-- stages are not read as P+'s, and Unranked's hidden rating is moved by P+ games only.
ALTER TABLE mm_matches ADD COLUMN game text NOT NULL DEFAULT '';
