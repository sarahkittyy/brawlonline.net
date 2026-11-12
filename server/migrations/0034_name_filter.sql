-- The name filter's rules (accounts::slur_filter, Sarah's board 2026-11-12): kept here, not in the
-- source, so the prompt and the words stay private. Edited with `pp-admin filter` (server/README.md).
-- A text is flagged (replaced by a kind one) when no `allow` rule matches it and a `word` or
-- `regex` rule matches it, or every model answers YES to an `ai` rule's prompt.
CREATE TABLE name_filter_rules (
    id         bigserial   PRIMARY KEY,
    -- ai: the model's system prompt (YES/NO per text).
    -- word: found anywhere in the folded text (lowercase, look-alike digits and symbols read as
    --       letters, everything but letters dropped: "G 1 r-M @" is "girma"); the word is folded too.
    -- regex: a case-insensitive regular expression, tried on the text as typed and folded.
    -- allow: a text (any case, spaces around ignored) never flagged: a false positive's fix.
    kind       text        NOT NULL CHECK (kind IN ('ai', 'word', 'regex', 'allow')),
    body       text        NOT NULL CHECK (body <> ''),
    note       text        NOT NULL DEFAULT '',
    -- Off: a draft that only `pp-admin filter test` uses. accounts reads the rules every 30 s.
    enabled    boolean     NOT NULL DEFAULT false,
    created_by text        NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

-- The model's answers, per prompt (prompt_hash: sha256 of the models and the prompt): the filter,
-- `filter test` and `user slur-scan` ask the model only about texts that prompt has not seen.
CREATE TABLE name_filter_answers (
    prompt_hash text        NOT NULL,
    -- name, code, room or social (accounts::slur_filter::What::key).
    what        text        NOT NULL,
    -- Trimmed and lowercase.
    text        text        NOT NULL,
    flagged     boolean     NOT NULL,
    asked_at    timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (prompt_hash, what, text)
);

-- Texts `pp-admin filter test` checks every rule against besides the players' own names: ones that
-- must be flagged and ones that must pass (an innocent word that looks like a slur).
CREATE TABLE name_filter_samples (
    id          bigserial   PRIMARY KEY,
    what        text        NOT NULL CHECK (what IN ('name', 'code', 'room', 'social')),
    text        text        NOT NULL CHECK (text <> ''),
    expect_flag boolean     NOT NULL,
    note        text        NOT NULL DEFAULT '',
    created_by  text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    UNIQUE (what, text)
);
