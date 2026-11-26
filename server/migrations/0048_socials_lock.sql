-- A socials lock (`pp-admin user lock-socials`, the launcher admin's rename box, 2026-11-26): the
-- account can't set or change its profile's social media accounts (`PUT /v1/me/profile` refuses
-- a new or changed one; removing one is allowed) or connect Twitch until `unlock-socials`. Only an admin sets it. NULL: not locked. Clearing the
-- accounts (`clear-socials`) is separate: it empties the profile's handles and drops the Twitch
-- connection, and needs no column.
ALTER TABLE users ADD COLUMN socials_locked_at timestamptz;
