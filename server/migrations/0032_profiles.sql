-- What a player puts on their profile (docs/design/profiles.md, Sarah's board 2026-11-09): mains
-- (common::profile::CHARACTERS keys, the first the primary), a country (common::countries code)
-- and social media accounts (each the handle common::profile::Platform::parse stores; NULL: not
-- set), and whether the flag and primary main show beside the name. One row once the player has set anything; none means an empty profile.
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
    -- The flag and the primary main beside the player's name around the launcher, not only on
    -- the profile (the leaderboard, the online list, rooms, chat).
    show_country boolean   NOT NULL DEFAULT true,
    show_main    boolean   NOT NULL DEFAULT true,
    updated_at timestamptz NOT NULL DEFAULT now()
);
