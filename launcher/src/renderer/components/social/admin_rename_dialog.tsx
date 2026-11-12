import LockOpenRoundedIcon from "@mui/icons-material/LockOpenRounded";
import LockRoundedIcon from "@mui/icons-material/LockRounded";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import type { SocialPlayer } from "@social/types";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { useToasts } from "@/lib/hooks/use_toasts";
import { useAdminRefresh } from "@/lib/social/admin_refresh";
import { validateDisplayName } from "@/lib/validate/validate";
import { useServices } from "@/services";
import type { AdminUser } from "@/services/backend/types";

import { AdminMessages as Messages } from "./admin.messages";
import styles from "./admin.module.css";

/** `abcd#0123` -> `ABCD#123`, as the server stores it; null for anything that isn't a code. */
export function canonicalCode(typed: string): string | null {
  const m = /^([A-Za-z]{1,4})#([0-9]{1,4})$/.exec(typed.trim());
  if (!m) {
    return null;
  }
  return `${m[1].toUpperCase()}#${Number(m[2])}`;
}

/** How long the code box waits after a key before asking who has the code. */
const CODE_CHECK_DELAY_MS = 350;

/**
 * An admin's rename box for a player (the player menu's Rename): their name and connect code filled in, the lock at
 * the bottom left (whether they may rename themselves), Apply and Cancel at the right. A code another account has
 * says whose it is and can't be applied (the server refuses it too).
 */
export const AdminRenameDialog = ({ player, onClose }: { player: SocialPlayer | null; onClose: () => void }) => (
  <Dialog open={player != null} onClose={onClose} maxWidth="xs" fullWidth={true}>
    {player && <RenameForm key={player.uid} player={player} onClose={onClose} />}
  </Dialog>
);

const RenameForm = ({ player, onClose }: { player: SocialPlayer; onClose: () => void }) => {
  const { backendService } = useServices();
  const { showError, showSuccess } = useToasts();
  const queryClient = useQueryClient();
  const bump = useAdminRefresh((s) => s.bump);
  const current = useQuery({
    queryKey: ["adminUser", player.uid],
    queryFn: () => backendService.adminUser(player.uid),
    staleTime: 0,
  });
  const [name, setName] = useState(player.displayName);
  const [code, setCode] = useState(player.connectCode);
  const [locked, setLocked] = useState<boolean | null>(null);
  const [saving, setSaving] = useState(false);
  const [refused, setRefused] = useState<string | null>(null);
  // The fields start from the list's copy and take the account's own once it is read (unless typed in already).
  const [touched, setTouched] = useState(false);
  const u = current.data;
  useEffect(() => {
    if (u && !touched) {
      setName(u.displayName);
      setCode(u.connectCode);
    }
  }, [u, touched]);
  const lockShown = locked ?? u?.nameLocked ?? false;

  const trimmedName = name.trim();
  const nameProblem = validateDisplayName(trimmedName);
  const canonical = canonicalCode(code);
  const origName = u?.displayName ?? player.displayName;
  const origCode = canonicalCode(u?.connectCode ?? player.connectCode);
  const codeChanged = canonical != null && canonical !== origCode;

  // Who has the code typed, asked a moment after the last key.
  const [checkCode, setCheckCode] = useState<string | null>(null);
  useEffect(() => {
    const t = setTimeout(() => setCheckCode(codeChanged ? canonical : null), CODE_CHECK_DELAY_MS);
    return () => clearTimeout(t);
  }, [canonical, codeChanged]);
  const owner = useQuery({
    queryKey: ["adminCodeOwner", checkCode],
    queryFn: () => backendService.adminCodeOwner(checkCode ?? ""),
    enabled: checkCode != null,
    staleTime: 0,
  });
  const checking = codeChanged && (checkCode !== canonical || owner.isFetching);
  const takenBy =
    codeChanged && owner.data && owner.data.code === canonical && owner.data.uid && owner.data.uid !== player.uid
      ? owner.data.displayName ?? owner.data.code
      : null;

  const nameChanged = trimmedName !== origName;
  const lockChanged = locked != null && locked !== (u?.nameLocked ?? false);
  const codeError = code.trim() !== "" && canonical == null ? Messages.codeFormat() : null;
  const changed = nameChanged || codeChanged || lockChanged;
  const canApply =
    !saving &&
    u != null &&
    changed &&
    nameProblem === true &&
    codeError == null &&
    // An account without a code keeps none (online play not set up); a blank box changes nothing.
    (code.trim() === "" ? origCode == null : true) &&
    !checking &&
    takenBy == null;

  const apply = async () => {
    if (!canApply) {
      return;
    }
    setSaving(true);
    setRefused(null);
    try {
      const saved: AdminUser = await backendService.adminUpdateUser(player.uid, {
        ...(nameChanged ? { displayName: trimmedName } : {}),
        ...(codeChanged && canonical ? { connectCode: canonical } : {}),
        ...(lockChanged && locked != null ? { nameLocked: locked } : {}),
      });
      queryClient.setQueryData(["adminUser", player.uid], saved);
      void queryClient.invalidateQueries({ queryKey: ["profile", player.uid] });
      bump();
      showSuccess(Messages.renamed(saved.displayName));
      onClose();
    } catch (err) {
      const e = err as { code?: string; message?: string };
      if (e.code === "code_taken" || e.code === "invalid_connect_code" || e.code === "invalid_display_name") {
        setRefused(e.message ?? String(err));
      } else {
        showError(err);
      }
    } finally {
      setSaving(false);
    }
  };

  const codeHelper = codeError ?? (takenBy ? Messages.codeTaken(takenBy) : checking ? Messages.checkingCode() : " ");

  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        void apply();
      }}
      data-testid="admin-rename"
    >
      <DialogTitle>{Messages.renameTitle()}</DialogTitle>
      <DialogContent>
        <div className={styles.fields}>
          <TextField
            label={Messages.name()}
            value={name}
            autoFocus={true}
            fullWidth={true}
            size="small"
            disabled={saving}
            inputProps={{ maxLength: 15, spellCheck: false, "data-testid": "admin-rename-name" }}
            error={nameProblem !== true}
            helperText={nameProblem !== true ? nameProblem : " "}
            onChange={(e) => {
              setTouched(true);
              setRefused(null);
              setName(e.target.value);
            }}
          />
          <TextField
            label={Messages.code()}
            value={code}
            fullWidth={true}
            size="small"
            disabled={saving}
            inputProps={{ maxLength: 9, spellCheck: false, "data-testid": "admin-rename-code" }}
            error={codeError != null || takenBy != null}
            helperText={codeHelper}
            onChange={(e) => {
              setTouched(true);
              setRefused(null);
              setCode(e.target.value.toUpperCase());
            }}
          />
        </div>
        {refused && (
          <p className={styles.refused} role="alert">
            {refused}
          </p>
        )}
        <div className={styles.actions}>
          <Tooltip
            title={lockShown ? Messages.nameLocked() : Messages.nameUnlocked()}
            placement="top"
            disableInteractive={true}
          >
            <span>
              <IconButton
                size="small"
                aria-label={lockShown ? Messages.unlockName() : Messages.lockName()}
                aria-pressed={lockShown}
                className={lockShown ? styles.locked : styles.unlocked}
                disabled={saving || u == null}
                onClick={() => setLocked(!lockShown)}
                data-testid="admin-rename-lock"
              >
                {lockShown ? <LockRoundedIcon fontSize="small" /> : <LockOpenRoundedIcon fontSize="small" />}
              </IconButton>
            </span>
          </Tooltip>
          <span className={styles.spacer} />
          {(saving || current.isLoading) && <CircularProgress size={16} />}
          <Button color="secondary" onClick={onClose} disabled={saving}>
            {Messages.cancel()}
          </Button>
          <Button
            color="primary"
            variant="contained"
            type="submit"
            disabled={!canApply}
            data-testid="admin-rename-apply"
          >
            {Messages.apply()}
          </Button>
        </div>
      </DialogContent>
    </form>
  );
};
