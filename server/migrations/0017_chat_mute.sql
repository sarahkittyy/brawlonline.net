-- A chat shadowban (`pp-admin mute`, 2026-10-21): the account's chat messages and DMs go nowhere,
-- while its own launcher and game show them as sent. NULL: not muted.
ALTER TABLE users ADD COLUMN chat_muted_at timestamptz;

-- mm and accounts read the accounts changed since their last look (`common::db::changed_users`)
-- every few seconds, so a ban, a rename or a mute reaches connected players at once.
CREATE INDEX users_updated_at_idx ON users (updated_at);

-- mm's role (0005_mm_grants.sql): fetch_mm_user reads chat_muted_at, changed_users filters on
-- updated_at.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT (chat_muted_at, updated_at) ON users TO pp_mm;
    END IF;
END
$$;
