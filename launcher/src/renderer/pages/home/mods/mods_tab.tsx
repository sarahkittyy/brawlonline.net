import type { ModBuild, ModBuildInput, ModRoomRequest, ModsState } from "@dolphin/mods/types";
import React, { useCallback, useEffect, useState } from "react";

import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useSettings } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useRoomList } from "@/lib/social/use_room_list";
import { useServices } from "@/services";

import type { AddBuildInitial } from "./add_build_dialog";
import { AddBuildDialog } from "./add_build_dialog";
import { ModsMessages as Messages } from "./mods.messages";
import type { ModsPublicRooms } from "./mods_view";
import { ModsView } from "./mods_view";

/** How long a list stays shown while polls fail (as Home > Overview). */
const STALE_AFTER_MS = 15_000;

const errorText = (err: unknown) => (err instanceof Error ? err.message : String(err));

/** Home > Other Games: the games, mods and the room, from the main process's ModsManager. */
export const ModsTab = React.memo(function ModsTab() {
  const { modsService } = useServices();
  const { showError, showSuccess } = useToasts();
  const isoPath = useSettings((store) => store.settings.isoPath);
  const [state, setState] = useState<ModsState | null>(null);
  const [hashProgress, setHashProgress] = useState<Record<string, number>>({});
  const [startingBuildId, setStartingBuildId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  // The add dialog: closed, a game (a disc), or a mod (found in a folder, on the Brawl disc).
  const [adding, setAdding] = useState<AddBuildInitial | null>(null);
  const { uid, query } = useRoomList();
  const openLogin = useLoginModal((s) => s.openModal);
  const outdated = query.isError && Date.now() - query.dataUpdatedAt > STALE_AFTER_MS;
  let publicRooms: ModsPublicRooms;
  if (!uid) {
    publicRooms = { kind: "logged-out" };
  } else if (query.data && !outdated) {
    publicRooms = { kind: "ready", rooms: query.data.modRooms ?? [] };
  } else if (query.isError) {
    publicRooms = { kind: "unreachable" };
  } else {
    publicRooms = { kind: "loading" };
  }

  useEffect(() => {
    let alive = true;
    void modsService.getState().then((s) => alive && setState(s));
    const stop = modsService.onEvent((event) => {
      if (event.type === "state") {
        setState(event.state);
      } else {
        const { id, done, total } = event.progress;
        setHashProgress((p) => {
          const next = { ...p };
          if (total > 0 && done >= total) {
            delete next[id];
          } else {
            next[id] = total > 0 ? done / total : 0;
          }
          return next;
        });
      }
    });
    return () => {
      alive = false;
      stop();
    };
  }, [modsService]);

  const start = useCallback(async (build: ModBuild, run: () => Promise<void>) => {
    setError(null);
    setStartingBuildId(build.id);
    try {
      await run();
    } catch (err) {
      setError(errorText(err));
    } finally {
      setStartingBuildId(null);
      setHashProgress((p) => {
        const next = { ...p };
        delete next[build.id];
        return next;
      });
    }
  }, []);

  const onAdd = async (input: ModBuildInput) => {
    try {
      await modsService.addBuild(input);
      setAdding(null);
    } catch (err) {
      showError(err);
    }
  };

  const onAddFolder = async () => {
    const result = await window.electron.common.showOpenDialog({ properties: ["openDirectory"] });
    const folder = result.canceled ? null : result.filePaths[0];
    if (!folder) {
      return;
    }
    try {
      const parts = await modsService.inspectFolder(folder);
      setAdding({
        kind: "mod",
        name: parts.name,
        discPath: isoPath,
        launcherPath: parts.launcherPath,
        sdPath: parts.sdPath,
      });
    } catch (err) {
      showError(err);
    }
  };

  const onRequest = (request: ModRoomRequest) => {
    void modsService.request(request).catch(showError);
  };

  return (
    <>
      <ModsView
        state={state}
        publicRooms={publicRooms}
        onLogIn={openLogin}
        hashProgress={hashProgress}
        startingBuildId={startingBuildId}
        error={error}
        onControllerChange={(build, controller) =>
          void modsService.updateBuild(build.id, { controller }).catch(showError)
        }
        onAddBuild={() => setAdding({ kind: "game", name: "", discPath: null, launcherPath: null, sdPath: null })}
        onAddFolder={() => void onAddFolder()}
        onRemoveBuild={(build) => void modsService.removeBuild(build.id).catch(showError)}
        onHost={(build, isPublic) => void start(build, () => modsService.host({ buildId: build.id, public: isPublic }))}
        onJoin={(build, code) => void start(build, () => modsService.join({ buildId: build.id, code }))}
        onRequest={onRequest}
        onCopyCode={(code) => {
          void navigator.clipboard.writeText(code).then(() => showSuccess(Messages.copied()));
        }}
      />
      <AddBuildDialog initial={adding} onClose={() => setAdding(null)} onAdd={onAdd} />
    </>
  );
});
