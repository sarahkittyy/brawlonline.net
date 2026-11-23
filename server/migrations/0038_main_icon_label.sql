-- The hover text of a player's custom main icon (docs/design/profiles.md §10), as a stock icon shows its
-- character's name; empty for none. pp-admin main-icon set --label.
ALTER TABLE user_main_icons ADD COLUMN label text NOT NULL DEFAULT '';
