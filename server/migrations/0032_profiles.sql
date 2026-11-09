-- What a player puts on their profile (docs/design/profiles.md, Sarah's board 2026-11-09): mains
-- (common::profile::CHARACTERS keys, the first the primary), a country (common::countries code)
-- and social media accounts (each the handle common::profile::Platform::parse stores; NULL: not
-- set). One row once the player has set anything; none means an empty profile.
CREATE TABLE profiles (
    uid        uuid        PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    mains      text[]      NOT NULL DEFAULT '{}',
    country    text        NULL,
    discord    text        NULL,
    twitter    text        NULL,
    twitch     text        NULL,
    youtube    text        NULL,
    tiktok     text        NULL,
    instagram  text        NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);
