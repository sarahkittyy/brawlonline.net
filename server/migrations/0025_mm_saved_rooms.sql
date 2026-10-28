-- Rooms across a restart of mm (bug report #178, 2026-10-28): mm keeps its rooms in memory, and a
-- deploy restarts it, so every room was gone ("Room not found.") when its members' games connected
-- again, some of them in the middle of the room's game. mm now saves its rooms here when it is
-- stopped and the next mm takes them back at its start (one row, the rooms as JSON: mm's own form,
-- `rooms.rs`); each member gets their slot back by joining the room again.
CREATE TABLE mm_saved_rooms (
    id smallint PRIMARY KEY CHECK (id = 1),
    saved_at timestamptz NOT NULL,
    rooms jsonb NOT NULL
);

-- mm's role (0005_mm_grants.sql): it writes the row when it stops and deletes it when it starts.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT, INSERT, UPDATE, DELETE ON mm_saved_rooms TO pp_mm;
    END IF;
END
$$;
