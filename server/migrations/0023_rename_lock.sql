-- A rename lock (`pp-admin user lock-name`, 2026-10-26): the account keeps its display name;
-- `/v1/me/rename` refuses until `unlock-name`. Only an admin sets it, never the slur filter.
-- NULL: not locked. mm never reads it.
ALTER TABLE users ADD COLUMN rename_locked_at timestamptz;
