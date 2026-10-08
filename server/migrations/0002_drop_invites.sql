-- Sign-up is open to everyone (user decision, 2026-10-08): invite codes are gone.
-- Drops the invites table and the column that recorded which invite an account used.

DROP TABLE invites;
ALTER TABLE users DROP COLUMN invite_code;
