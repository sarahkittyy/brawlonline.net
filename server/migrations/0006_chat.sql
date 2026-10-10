-- Chat (docs/chat-protocol.md): the identity keys mm has seen, and the reports players sent.
--
-- chat_identity_keys: each install's Ed25519 public key as `hello` carried it (mm keeps the 20
-- most recent per account). A report's messages are checked against the reported account's keys.
CREATE TABLE chat_identity_keys (
    uid         uuid        NOT NULL REFERENCES users(uid) ON DELETE CASCADE,
    public_key  bytea       NOT NULL CHECK (length(public_key) = 32),
    first_seen  timestamptz NOT NULL DEFAULT now(),
    last_seen   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (uid, public_key)
);

-- chat_reports: what a player reported (`chat-report`). `messages` is a JSON list of
-- {seq, text (cleaned), raw (the signed text as hex), sig, verified}; `verified` counts the
-- messages whose signature checks out against one of the reported account's identity keys.
-- `admin chat-reports` lists the unhandled ones, `admin chat-report-done` sets handled_at.
CREATE TABLE chat_reports (
    id          bigserial   PRIMARY KEY,
    reporter    uuid        NULL REFERENCES users(uid) ON DELETE SET NULL,
    reported    uuid        NULL REFERENCES users(uid) ON DELETE SET NULL,
    group_id    text        NOT NULL,
    reason      text        NOT NULL DEFAULT '',
    messages    jsonb       NOT NULL,
    verified    integer     NOT NULL,
    total       integer     NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    handled_at  timestamptz NULL
);
CREATE INDEX chat_reports_unhandled_idx ON chat_reports (created_at DESC) WHERE handled_at IS NULL;

-- mm's role (0005_mm_grants.sql): only what common::db's chat queries need. record_chat_key
-- upserts (INSERT ... ON CONFLICT DO UPDATE last_seen) and prunes (DELETE with a SELECT);
-- fetch_chat_keys reads; insert_chat_report inserts without RETURNING (the id comes from the
-- sequence).
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT, INSERT, DELETE, UPDATE (last_seen) ON chat_identity_keys TO pp_mm;
        GRANT INSERT (reporter, reported, group_id, reason, messages, verified, total) ON chat_reports TO pp_mm;
        GRANT USAGE ON SEQUENCE chat_reports_id_seq TO pp_mm;
    END IF;
END
$$;
