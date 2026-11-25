-- The launcher's changelogs (docs/design/changelog.md). Sarah writes one in her agent board, which
-- publishes it with `pp-admin changelog publish` as it pushes to main; a launcher that was just
-- updated shows the newest one once. Older ones stay as the history.
CREATE TABLE changelogs (
    id           bigserial   PRIMARY KEY,
    -- Markdown, as the blog writes it (accounts::blog::render); its pictures are /blog/media/<name>.
    body_md      text        NOT NULL,
    -- Which one is the newest; launchers built before it never show it (accounts::changelog).
    published_at timestamptz NOT NULL DEFAULT now(),
    -- Staging only (the board's "show on staging"): shown at every start of every launcher, ahead
    -- of the others. `publish --every-start` keeps one at most.
    every_start  boolean     NOT NULL DEFAULT false
);

CREATE INDEX changelogs_published ON changelogs (every_start DESC, published_at DESC);
