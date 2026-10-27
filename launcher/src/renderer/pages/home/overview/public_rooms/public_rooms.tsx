import React, { useState } from "react";

import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useRememberedRuleset } from "@/lib/rulesets/use_remembered_ruleset";
import { useCreateRoom } from "@/lib/social/use_create_room";
import { useRoomList } from "@/lib/social/use_room_list";
import { useRoomName } from "@/lib/social/use_room_name";
import { RulesetPreviewDialog } from "@/pages/rulesets/ruleset_preview_dialog";

import type { PublicRoomsState } from "./public_rooms_view";
import { PublicRoomsView } from "./public_rooms_view";
import { RulesetBox } from "./ruleset_box";
import { useJoinRoom } from "./use_join_room";

/** How long a list stays shown while polls fail. */
const STALE_AFTER_MS = 15_000;
/** Set once the player clicks "Create room"; until then the button glows. */
const CREATED_ROOM_STORAGE_KEY = "HAS_CREATED_ROOM";

/** Players online and the public rooms, polled every few seconds while Home > Overview is open. */
export const PublicRooms = React.memo(function PublicRooms() {
  const { uid, query } = useRoomList();
  const openLogin = useLoginModal((s) => s.openModal);
  const { join, joiningCode } = useJoinRoom();
  const { create: createRoom, creating } = useCreateRoom();
  const { ruleset } = useRememberedRuleset();
  const { rename, shown } = useRoomName("pplus");
  /** The ruleset whose rules are shown (a click on a room's ruleset). */
  const [showRuleset, setShowRuleset] = useState<string | null>(null);
  /** The join in progress came from the room code box. */
  const [joiningByCode, setJoiningByCode] = useState(false);
  const [hasCreatedRoom, setHasCreatedRoom] = useLocalStorage(CREATED_ROOM_STORAGE_KEY, false);
  // The room starts with the ruleset in the box (docs/rulesets-protocol.md §4).
  const create = () => {
    setHasCreatedRoom(true);
    void createRoom(ruleset?.code);
  };

  // A failed poll keeps the last list for a little while (the next poll usually replaces it);
  // after that the list is too old to click on.
  const outdated = query.isError && Date.now() - query.dataUpdatedAt > STALE_AFTER_MS;
  let state: PublicRoomsState;
  if (!uid) {
    state = { kind: "logged-out" };
  } else if (query.data && !outdated) {
    // A name just given shows at once, before mm has it in the list.
    const rooms = query.data.rooms.map((r) => ({ ...r, name: shown(r.code, r.name) }));
    state = { kind: "ready", online: query.data.online, rooms };
  } else if (query.isError) {
    state = { kind: "unreachable" };
  } else {
    state = { kind: "loading" };
  }

  const yourRoom = query.data?.yourRoom?.kind === "pplus" ? query.data.yourRoom : null;

  return (
    <>
      <PublicRoomsView
        state={state}
        yourRoom={yourRoom}
        onRenameRoom={rename}
        joiningCode={joiningCode}
        onJoin={(room) => {
          if (room.joinable) {
            void join(room.code);
          }
        }}
        onJoinCode={async (code) => {
          setJoiningByCode(true);
          try {
            return await join(code);
          } finally {
            setJoiningByCode(false);
          }
        }}
        joiningByCode={joiningByCode}
        onRetry={() => void query.refetch()}
        onLogIn={openLogin}
        onCreate={create}
        creating={creating}
        rulesetBox={<RulesetBox />}
        highlightCreate={!hasCreatedRoom}
        onShowRuleset={(room) => {
          if (room.ruleset?.code) {
            setShowRuleset(room.ruleset.code);
          }
        }}
      />
      <RulesetPreviewDialog open={showRuleset !== null} code={showRuleset} onClose={() => setShowRuleset(null)} />
    </>
  );
});
