-- Room names (2026-10-27, docs/design/rooms.md §1.4): the name the account gives the rooms it
-- hosts, one for P+ rooms and one for Brawl Mods (Other Games) rooms. The launcher sets it
-- (`PUT /v1/me/room-name`, through the racial slur filter); mm reads it when the account creates a
-- room and, through `changed_users`, renames the rooms it hosts. NULL: no name ("alice's room").
ALTER TABLE users ADD COLUMN room_name text CHECK (char_length(room_name) BETWEEN 1 AND 32);
ALTER TABLE users ADD COLUMN mods_room_name text CHECK (char_length(mods_room_name) BETWEEN 1 AND 32);

-- mm's role (0005_mm_grants.sql): fetch_mm_user and changed_users read them.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'pp_mm') THEN
        GRANT SELECT (room_name, mods_room_name) ON users TO pp_mm;
    END IF;
END
$$;
