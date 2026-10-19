-- SD card listings for Other Games rooms (docs/design/brawl-mods.md §6.5, the user's, 2026-10-19:
-- the launcher asks about a room's SD card before it ever starts Dolphin, naming the files that
-- differ and their size). A host's launcher puts its card's listing here when it hosts: the
-- card's canonical manifest (a hash per 64 KiB chunk) and its file list, as DolphinTool `sdhash
-- --listing` writes it. A joiner's launcher fetches it by the room's card root and compares it
-- with its own card (DolphinTool `sdcompare`). Stored once per root; the server checks that the
-- manifest hashes to it. Deleted when unused for 90 days.
CREATE TABLE sd_card_listings (
    root         text        PRIMARY KEY CHECK (root ~ '^[0-9a-f]{64}$'),
    listing      bytea       NOT NULL,
    bytes        bigint      NOT NULL CHECK (bytes > 0),
    uploaded_by  uuid        NULL REFERENCES users (uid) ON DELETE SET NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    used_at      timestamptz NOT NULL DEFAULT now()
);
