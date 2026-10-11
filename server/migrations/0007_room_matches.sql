-- Room games in the match history (docs/rooms-protocol.md §3). mm records every room start whose
-- tickets all came in, as mode 3 (Slippi's Teams, the room ticket's mode), with the room's code,
-- its mode at the start and each player's slot (the in-game port) and team, in the order of
-- `players`. Every player's game reports the game (POST /v1/ranked/report-game) with each
-- player's place; nothing about a room game is rated.
--
-- pp_mm's table-level INSERT on mm_matches (0005_mm_grants.sql) covers the new columns.

ALTER TABLE mm_matches
    ADD COLUMN room_code text     NULL,
    ADD COLUMN room_mode text     NULL CHECK (room_mode IN ('1v1', 'ffa', 'teams')),
    -- 1-4, one per player.
    ADD COLUMN ports     smallint[] NULL,
    -- 0 red, 1 blue, 2 green, one per player; NULL unless the room played a team battle.
    ADD COLUMN teams     smallint[] NULL;
