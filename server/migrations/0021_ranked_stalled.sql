-- A Ranked set's player's game that stopped running while it stayed connected to mm (bug report
-- #105, 2026-10-22): the opponent's game froze 18 s into a game and kept its online connection up
-- for 19 minutes, so the other player's "opponent left" waited all that time (an "opponent left"
-- about a connected game blames nobody) and would have been orphaned at 30. Games now send
-- `game-alive` while their emulation runs; mm sets stalled_since (the last one) once a game that
-- sends them sent none for 30 s, and clears it when they come again. A report made after
-- stalled_since is about a frozen game (common::ranked::resolve).
--
-- pp_mm's table grants (0018) cover the new column.

ALTER TABLE ranked_presence ADD COLUMN stalled_since timestamptz;
