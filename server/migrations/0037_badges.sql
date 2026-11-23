-- Badges and custom main icons (docs/design/profiles.md §10, Sarah's board 2026-11-17). Only
-- pp-admin writes these tables; accounts reads them for the names around the launcher and the
-- profile page, and serves the icons at /v1/icons/<id>.png.

-- PNG icons (common::badges::check_png: at most 64 KiB and 256x256). id is the start of the
-- file's SHA-256 (common::badges::icon_id), so the same file is stored once; name is what
-- pp-admin calls it.
CREATE TABLE icons (
    id          text        PRIMARY KEY,
    name        text        NOT NULL UNIQUE,
    png         bytea       NOT NULL,
    width       integer     NOT NULL,
    height      integer     NOT NULL,
    uploaded_by text        NOT NULL DEFAULT '',
    created_at  timestamptz NOT NULL DEFAULT now()
);

-- Badges before a player's name, in pos order. places: common::badges::Place names; NULL shows
-- the badge everywhere.
CREATE TABLE user_badges (
    uid        uuid        NOT NULL REFERENCES users (uid) ON DELETE CASCADE,
    icon       text        NOT NULL REFERENCES icons (id),
    label      text        NOT NULL DEFAULT '',
    pos        integer     NOT NULL,
    places     text[]      NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (uid, icon)
);

-- The picture shown in place of a player's primary main's stock icon, whatever their own
-- "Show next to name" switch says.
CREATE TABLE user_main_icons (
    uid        uuid        PRIMARY KEY REFERENCES users (uid) ON DELETE CASCADE,
    icon       text        NOT NULL REFERENCES icons (id),
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX user_main_icons_icon ON user_main_icons (icon);
CREATE INDEX user_badges_icon ON user_badges (icon);
