-- How many people read each post (docs/design/blog.md, "Views"): one view per reader per post per
-- UTC day, counted by accounts when it serves the page to a browser (bots and link previews
-- don't count, nor a draft's preview link). Shown after the date on the index and in the post's
-- byline.
ALTER TABLE blog_posts ADD COLUMN views bigint NOT NULL DEFAULT 0;

-- Who has already been counted today: a keyed hash of the reader (their address, /64 for IPv6, and
-- browser), never the address itself. Rows older than yesterday are deleted as views come in.
CREATE TABLE blog_post_view_seen (
    post_id bigint NOT NULL REFERENCES blog_posts (id) ON DELETE CASCADE,
    day     date   NOT NULL,
    visitor bytea  NOT NULL,
    PRIMARY KEY (post_id, day, visitor)
);

CREATE INDEX blog_post_view_seen_day ON blog_post_view_seen (day);
