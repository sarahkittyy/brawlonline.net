-- The blog (brawlonline.net/blog, docs/design/blog.md). Admins (users.role = 'admin') write posts in
-- the web editor (/blog/write/); accounts renders the pages from these rows.
CREATE TABLE blog_posts (
    id           bigserial   PRIMARY KEY,
    -- The address: /blog/<slug>. Lowercase letters, digits and dashes (accounts::blog::check_slug).
    slug         text        NOT NULL UNIQUE,
    title        text        NOT NULL DEFAULT '',
    -- The line under the title on the index and in link previews; empty: the first paragraph.
    summary      text        NOT NULL DEFAULT '',
    -- The byline ("by <author>"); empty: none.
    author       text        NOT NULL DEFAULT '',
    body_md      text        NOT NULL DEFAULT '',
    status       text        NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'published')),
    -- Set at the first publish and kept through unpublish/publish; the date the pages show.
    published_at timestamptz NULL,
    -- A draft's preview link: /blog/<slug>?preview=<token>.
    preview_token text       NOT NULL,
    created_by   uuid        NULL REFERENCES users (uid) ON DELETE SET NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX blog_posts_published ON blog_posts (published_at DESC) WHERE status = 'published';

-- Images and videos uploaded from the editor. The file is BLOG_MEDIA_DIR/<name>, served at
-- /blog/media/<name>; <name> is the start of the content's SHA-256 and its type's extension, so the
-- same file uploaded twice is stored once.
CREATE TABLE blog_media (
    name          text        PRIMARY KEY,
    kind          text        NOT NULL CHECK (kind IN ('image', 'video')),
    bytes         bigint      NOT NULL,
    original_name text        NOT NULL DEFAULT '',
    uploaded_by   uuid        NULL REFERENCES users (uid) ON DELETE SET NULL,
    created_at    timestamptz NOT NULL DEFAULT now()
);
