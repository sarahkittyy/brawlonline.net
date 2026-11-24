-- Twitch accounts connected through Twitch's own sign-in (docs/design/twitch-live.md). A connected
-- player who streams while they play shows as live around the launcher; a Twitch handle typed into
-- the profile never does. One Twitch account belongs to one Brawl Online account: connecting it
-- from another account moves it there.
--
-- 0045, not 0039: the tournaments and EmissaryRecomp branches hold 0039-0044.
CREATE TABLE twitch_links (
    uid          uuid        PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    -- Twitch's numeric user id (as text, as Helix sends it): the login can be renamed, the id not.
    twitch_id    text        NOT NULL UNIQUE,
    -- The login (lowercase, the twitch.tv/<login> address) and the display name, as of the last
    -- connect or the last stream seen.
    login        text        NOT NULL,
    display_name text        NOT NULL,
    linked_at    timestamptz NOT NULL DEFAULT now()
);
