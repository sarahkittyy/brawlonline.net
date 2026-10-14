-- Ranked tiers (docs/design/ranked-tiers.md, common::ranked): each player's rank and rating
-- deviation, and both per rating change. rank is a common::ranked::Rank index (0 Bronze 3 ... 18
-- Legendary), NULL while placing; rd is Glicko's rating deviation, which only decides placing.
-- accounts (and pp-admin migrate) fill them from rating_events right after this migration;
-- pp-admin ranked recompute fills them again after a cutoff change or a manual correction.
ALTER TABLE ratings
    ADD COLUMN rank  smallint         NULL CHECK (rank BETWEEN 0 AND 18),
    ADD COLUMN rd    double precision NULL;

ALTER TABLE rating_events
    ADD COLUMN rank_before  smallint         NULL,
    ADD COLUMN rank_after   smallint         NULL,
    ADD COLUMN rd_after     double precision NULL;
