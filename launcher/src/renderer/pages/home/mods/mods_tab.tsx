import type {
  ModBuild,
  ModBuildInput,
  ModRoomRequest,
  ModSaveInfo,
  ModSdCardCheck,
  ModsSdChoice,
  ModsState,
} from "@dolphin/mods/types";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import React, { useCallback, useEffect, useRef, useState } from "react";

import { usePlayDialogs } from "@/lib/dolphin/use_play";
import { useAccount } from "@/lib/hooks/use_account";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useSettings } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useRoomList } from "@/lib/social/use_room_list";
import { useServices } from "@/services";

import type { AddBuildInitial, SaveSectionProps } from "./add_build_dialog";
import { AddBuildDialog, formatSaveSummary } from "./add_build_dialog";
import { ModsMessages as Messages } from "./mods.messages";
import type { ModsPublicRooms } from "./mods_view";
import { ModsView, SD_BIG_DIFFERENCE_BYTES, SdCardDialog } from "./mods_view";

/** How long a list stays shown while polls fail (as Home > Overview). */
const STALE_AFTER_MS = 15_000;

// The main process's message, without Electron's "Error invoking remote method '…': Error: ".
const errorText = (err: unknown) =>
  (err instanceof Error ? err.message : String(err)).replace(
    /^Error invoking remote method '[^']*': (\w*Error: )?/,
    "",
  );

/** Home > Other Games: the games, mods and the room, from the main process's ModsManager. */
export const ModsTab = React.memo(function ModsTab() {
  const { modsService, backendService } = useServices();
  const currentUser = useAccount((store) => store.user);
  const userData = useAccount((store) => store.userData);
  const { setActivateOnlineOpen } = usePlayDialogs();
  const { showError, showSuccess } = useToasts();
  const isoPath = useSettings((store) => store.settings.isoPath);
  const [state, setState] = useState<ModsState | null>(null);
  const [hashProgress, setHashProgress] = useState<Record<string, number | null>>({});
  const [startingBuildId, setStartingBuildId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  // The add dialog: closed, a game (a disc), a mod (found in a folder, on the Brawl disc), or a
  // game being edited (editingId).
  const [adding, setAdding] = useState<AddBuildInitial | null>(null);
  const [editingId, setEditingId] = useState<string | null>(null);
  const { uid, query } = useRoomList();
  // The edited Wii game's save in rooms (null: no save section), and a save picked to replace it.
  const [saveInfo, setSaveInfo] = useState<SaveSectionProps["info"] | null>(null);
  const [saveBusy, setSaveBusy] = useState(false);
  const [replacing, setReplacing] = useState<{ source: string; current: ModSaveInfo } | null>(null);
  // The game whose save is being read: an answer for another one (opened before) is dropped.
  const saveFor = useRef<string | null>(null);
  // A room whose game the player does not have (a code typed in, or a public room).
  const [missing, setMissing] = useState<{ name: string; hash: string } | null>(null);
  // A room whose SD card differs from the player's: asked before Dolphin starts (sd_listing.ts).
  const [sdAsk, setSdAsk] = useState<{ buildId: string; code: string; check: ModSdCardCheck } | null>(null);
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
    // The hashes running come with the state (a check started before this tab opened shows), and
    // their progress between states as events.
    void modsService.getState().then((s) => {
      if (alive) {
        setState(s);
        setHashProgress(s.hashing);
      }
    });
    const stop = modsService.onEvent((event) => {
      if (event.type === "state") {
        setState(event.state);
        setHashProgress(event.state.hashing);
      } else if (event.type === "duplicate") {
        showError(Messages.duplicate(event.name));
      } else if (event.type === "hashFailed") {
        showError(Messages.hashFailed(event.name, event.error));
      } else {
        const { id, fraction } = event.progress;
        setHashProgress((p) => ({ ...p, [id]: fraction }));
      }
    });
    return () => {
      alive = false;
      stop();
    };
  }, [modsService, showError]);

  // A room needs the player's login in Dolphin (its online connection): logged in, with an online
  // code, and the play key on disk, as Play makes sure (use_play.ts).
  const ensureOnline = useCallback(async (): Promise<boolean> => {
    if (!currentUser) {
      openLogin();
      return false;
    }
    if (!userData?.playKey) {
      setActivateOnlineOpen(true);
      return false;
    }
    await backendService.assertPlayKey(userData.playKey);
    return true;
  }, [currentUser, userData, openLogin, setActivateOnlineOpen, backendService]);

  const start = useCallback(
    async (build: ModBuild, run: () => Promise<void>) => {
      setError(null);
      try {
        if (!(await ensureOnline())) {
          return;
        }
      } catch (err) {
        setError(errorText(err));
        return;
      }
      setStartingBuildId(build.id);
      try {
        await run();
      } catch (err) {
        setError(errorText(err));
      } finally {
        setStartingBuildId(null);
      }
    },
    [ensureOnline],
  );

  // Joins a room; one whose SD card differs comes back to ask first (Dolphin starts on Yes).
  const joinRoom = async (buildId: string, code: string, sdSync?: ModsSdChoice) => {
    const result = await modsService.join({ buildId, code, uid: uid ?? undefined, sdSync });
    if (result.outcome === "sd-card") {
      setSdAsk({ buildId: result.buildId, code: result.code, check: result.check });
    }
  };

  // A code typed by the public rooms: the room's game is looked up, then joined with the player's
  // game with the same hash (or the popup says there is none).
  const joinCode = async (code: string) => {
    setError(null);
    try {
      if (!uid || !(await ensureOnline())) {
        return;
      }
      const result = await modsService.joinByCode({ uid, code });
      if (result.outcome === "no-game") {
        setMissing({ name: result.name, hash: result.hash });
      } else if (result.outcome === "sd-card") {
        setSdAsk({ buildId: result.buildId, code: result.code, check: result.check });
      }
    } catch (err) {
      setError(errorText(err));
    }
  };

  const sdAskBody = (check: ModSdCardCheck) => {
    if (check.kind !== "differs") {
      return Messages.sdAskBodyUnknown();
    }
    const mb = Messages.megabytes(check.bytes);
    return check.filesKnown ? Messages.sdAskBody(mb, check.files) : Messages.sdAskBodyNoFiles(mb);
  };

  const onSdAnswer = (choice: "yes" | "overwrite" | "no") => {
    const ask = sdAsk;
    setSdAsk(null);
    const build = ask && state?.builds.find((b) => b.id === ask.buildId);
    if (!ask || !build || choice === "no") {
      return;
    }
    void start(build, () => joinRoom(ask.buildId, ask.code, choice));
  };

  const onAdd = async (input: ModBuildInput) => {
    try {
      if (editingId) {
        await modsService.updateBuild(editingId, input);
      } else {
        await modsService.addBuild(input);
      }
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
      setEditingId(null);
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

  // The edit window: the game's files, and a Wii game's save.
  const openEdit = (build: ModBuild, highlight?: AddBuildInitial["highlight"]) => {
    setEditingId(build.id);
    // A name the launcher picked shows empty, so it keeps following the game's files.
    setAdding({
      kind: "edit",
      name: build.autoName ? "" : build.name,
      discPath: build.discPath,
      launcherPath: build.launcherPath ?? null,
      sdPath: build.sdPath ?? null,
      highlight,
    });
    saveFor.current = build.id;
    if (build.platform === "gamecube") {
      setSaveInfo(null);
      return;
    }
    setSaveInfo("loading");
    modsService
      .saveInfo(build.id)
      .then((info) => saveFor.current === build.id && setSaveInfo(info ?? "not-ready"))
      .catch((err) => {
        if (saveFor.current === build.id) {
          setSaveInfo(null);
          showError(errorText(err));
        }
      });
  };

  const runImport = async (source: string) => {
    if (!editingId) {
      return;
    }
    setSaveBusy(true);
    try {
      const result = await modsService.importSave(editingId, source);
      if (saveFor.current === editingId) {
        setSaveInfo(result.save);
      }
      showSuccess(Messages.saveImported());
    } catch (err) {
      showError(errorText(err));
    } finally {
      setSaveBusy(false);
    }
  };

  const onImportSave = async (from: "file" | "folder") => {
    const result = await window.electron.common.showOpenDialog(
      from === "file"
        ? { properties: ["openFile"], filters: [{ name: Messages.saveFileFilter(), extensions: ["bin"] }] }
        : { properties: ["openDirectory"] },
    );
    const source = result.canceled ? null : result.filePaths[0];
    if (!source) {
      return;
    }
    // A save the game has already is replaced only once the player says so (it is kept aside).
    if (saveInfo && typeof saveInfo === "object" && saveInfo.files > 0) {
      setReplacing({ source, current: saveInfo });
      return;
    }
    await runImport(source);
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
        onControllerChange={(controller) => void modsService.setController(controller).catch(showError)}
        onConfigureControllers={() => void modsService.configureControllers().catch(showError)}
        onRefreshControllers={() => void modsService.refreshControllers().catch(() => undefined)}
        onAddBuild={() => {
          setEditingId(null);
          setAdding({ kind: "game", name: "", discPath: null, launcherPath: null, sdPath: null });
        }}
        onEditBuild={(build) => openEdit(build)}
        onAddFolder={() => void onAddFolder()}
        onRemoveBuild={(build) => void modsService.removeBuild(build.id).catch(showError)}
        onHost={(build, isPublic) =>
          void start(build, () => modsService.host({ buildId: build.id, public: isPublic, uid: uid ?? undefined }))
        }
        onJoin={(build, code) => void start(build, () => joinRoom(build.id, code))}
        onJoinCode={(code) => void joinCode(code)}
        onMissingGame={(name, hash) => setMissing({ name, hash })}
        // The room's part that differs: the game's edit window, that field pulsing gold.
        onFixBuild={(build, part) => openEdit(build, part)}
        onRequest={onRequest}
        onCopyCode={(code) => {
          void navigator.clipboard.writeText(code).then(() => showSuccess(Messages.copied()));
        }}
      />
      <AddBuildDialog
        initial={adding}
        onClose={() => setAdding(null)}
        onAdd={onAdd}
        save={
          saveInfo && editingId
            ? {
                info: saveInfo,
                busy: saveBusy,
                onImport: (from) => void onImportSave(from).catch((err) => showError(errorText(err))),
                onOpenFolder: () =>
                  void modsService.openSaveFolder(editingId).catch((err) => showError(errorText(err))),
              }
            : null
        }
      />
      <Dialog open={replacing !== null} onClose={() => setReplacing(null)} maxWidth="xs" fullWidth={true}>
        <DialogTitle>{Messages.replaceSaveTitle()}</DialogTitle>
        <DialogContent>{replacing && Messages.replaceSave(formatSaveSummary(replacing.current))}</DialogContent>
        <DialogActions>
          <Button onClick={() => setReplacing(null)}>{Messages.cancel()}</Button>
          <Button
            variant="contained"
            onClick={() => {
              const source = replacing?.source;
              setReplacing(null);
              if (source) {
                void runImport(source);
              }
            }}
          >
            {Messages.replace()}
          </Button>
        </DialogActions>
      </Dialog>
      <SdCardDialog
        open={sdAsk !== null}
        body={sdAsk ? sdAskBody(sdAsk.check) : ""}
        bigDifference={sdAsk?.check.kind === "differs" && sdAsk.check.bytes > SD_BIG_DIFFERENCE_BYTES}
        names={sdAsk?.check.kind === "differs" ? sdAsk.check.names : []}
        files={sdAsk?.check.kind === "differs" ? sdAsk.check.files : 0}
        onAnswer={onSdAnswer}
      />
      <Dialog open={missing !== null} onClose={() => setMissing(null)} maxWidth="xs" fullWidth={true}>
        <DialogTitle>{Messages.noMatchingGameTitle()}</DialogTitle>
        <DialogContent>{missing && Messages.noMatchingGame(missing.name, missing.hash)}</DialogContent>
        <DialogActions>
          <Button onClick={() => setMissing(null)}>{Messages.ok()}</Button>
        </DialogActions>
      </Dialog>
    </>
  );
});
