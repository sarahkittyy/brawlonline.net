-- Bug reports from the launcher's bug button (docs: server/README.md, "Bug reports").
--
-- The text comes first (`POST /v1/bug-reports`), the logs zip after it
-- (`PUT /v1/bug-reports/<id>/logs`, with the one-time upload token the first call answered), so a
-- report whose zip never arrives still has its text. The zip is a file, `bug-<id>.zip` in
-- BUG_REPORTS_DIR (on prod /var/lib/ppserver/bug-reports), not a row: logs are megabytes and the
-- database backups should not carry them.
--
-- uid: the account that was logged in (NULL when nobody was, or the account is gone).
-- upload_token_hash: sha256 of the upload token; NULL once the zip is in (or never sent).
-- logs_bytes: the zip's size once it is in, else NULL.
-- `admin bug list` shows the unresolved ones, `admin bug resolve` sets resolved_at,
-- `admin bug delete` / `admin bug clear` remove rows and their zips.
CREATE TABLE bug_reports (
    id                 bigserial   PRIMARY KEY,
    uid                uuid        NULL REFERENCES users(uid) ON DELETE SET NULL,
    description        text        NOT NULL,
    launcher_version   text        NOT NULL DEFAULT '',
    platform           text        NOT NULL DEFAULT '',
    upload_token_hash  bytea       NULL,
    logs_bytes         bigint      NULL,
    created_at         timestamptz NOT NULL DEFAULT now(),
    resolved_at        timestamptz NULL,
    resolution         text        NOT NULL DEFAULT ''
);
CREATE INDEX bug_reports_open_idx ON bug_reports (created_at DESC) WHERE resolved_at IS NULL;
