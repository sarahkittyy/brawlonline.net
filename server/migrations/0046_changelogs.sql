-- The launcher's changelogs (docs/design/changelog.md). Admins (users.role = 'admin') write them in
-- the web editor (/changelog/write/); a launcher that was just updated shows the newest published
-- one once. Older ones stay as the history.
CREATE TABLE changelogs (
    id           bigserial   PRIMARY KEY,
    body_md      text        NOT NULL DEFAULT '',
    status       text        NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'published')),
    -- Set at the first publish and kept through unpublish/publish: which one is the newest, and
    -- launchers built before it never show it (accounts::changelog).
    published_at timestamptz NULL,
    created_by   uuid        NULL REFERENCES users (uid) ON DELETE SET NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX changelogs_published ON changelogs (published_at DESC) WHERE status = 'published';
