-- Phase 1 schema: accounts, sessions, invites, email tokens, audit log, match records.
-- See docs/backend-design.md section 2.2.

CREATE TABLE users (
    uid               uuid        PRIMARY KEY,
    -- Stored trimmed and lowercased; the unique index makes it the login name.
    email             text        NOT NULL UNIQUE,
    email_verified_at timestamptz NULL,
    pw_hash           text        NOT NULL,
    display_name      text        NOT NULL,
    -- Assigned once by POST /v1/me/netplay, immutable afterwards (admin can change).
    connect_code      text        NULL UNIQUE,
    -- The play key is derived from (secret, uid, version); bump to rotate.
    play_key_version  integer     NOT NULL DEFAULT 1,
    rules_version     integer     NOT NULL DEFAULT 0,
    role              text        NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'admin')),
    banned_until      timestamptz NULL,
    ban_reason        text        NULL,
    invite_code       text        NULL,
    created_at        timestamptz NOT NULL DEFAULT now(),
    updated_at        timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE sessions (
    token_hash  bytea       PRIMARY KEY,
    uid         uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    created_at  timestamptz NOT NULL DEFAULT now(),
    last_seen   timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL,
    user_agent  text        NULL
);
CREATE INDEX sessions_uid_idx ON sessions (uid);

CREATE TABLE invites (
    code        text        PRIMARY KEY,
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NULL,
    max_uses    integer     NOT NULL DEFAULT 1 CHECK (max_uses > 0),
    uses        integer     NOT NULL DEFAULT 0,
    note        text        NULL,
    created_by  text        NOT NULL,
    revoked_at  timestamptz NULL
);

-- One-time tokens sent by email (verification, password reset) or printed by
-- the admin CLI (password reset).
CREATE TABLE email_tokens (
    token_hash  bytea       PRIMARY KEY,
    uid         uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    purpose     text        NOT NULL CHECK (purpose IN ('verify_email', 'reset_password')),
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL,
    used_at     timestamptz NULL
);
CREATE INDEX email_tokens_uid_idx ON email_tokens (uid, purpose);

CREATE TABLE audit_log (
    id      bigserial   PRIMARY KEY,
    actor   text        NOT NULL,
    action  text        NOT NULL,
    target  text        NULL,
    detail  jsonb       NOT NULL DEFAULT '{}'::jsonb,
    at      timestamptz NOT NULL DEFAULT now()
);

-- Written by mm when it pairs players. Status values follow Slippi's
-- ASSIGNED, COMPLETE, ABANDONED, ORPHANED, TERMINATED, ERROR.
CREATE TABLE mm_matches (
    match_id    text        PRIMARY KEY,
    mode        smallint    NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    players     uuid[]      NOT NULL,
    is_host     uuid        NOT NULL,
    stages      smallint[]  NOT NULL DEFAULT '{}',
    region      text        NULL,
    status      text        NOT NULL DEFAULT 'ASSIGNED'
);
CREATE INDEX mm_matches_created_idx ON mm_matches (created_at);
