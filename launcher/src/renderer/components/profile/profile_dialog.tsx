import BlockIcon from "@mui/icons-material/Block";
import CheckIcon from "@mui/icons-material/Check";
import CloseIcon from "@mui/icons-material/Close";
import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import MailOutlinedIcon from "@mui/icons-material/MailOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { clsx } from "clsx";
import React, { useMemo, useState } from "react";

import { useAccount } from "@/lib/hooks/use_account";
import type { AccountDialog } from "@/lib/hooks/use_account_dialogs";
import { useAccountDialogs } from "@/lib/hooks/use_account_dialogs";
import { useOwnProfileHint } from "@/lib/hooks/use_own_profile_hint";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useSocial } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";
import { useServices } from "@/services";
import type { PlayerProfile, ProfileUpdate } from "@/services/backend/types";

import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";
import type { ProfileEditing } from "./profile_view";
import { ProfileView } from "./profile_view";

/**
 * View Profile (docs/design/profiles.md): anyone's public profile, over whatever page is open. Opened with
 * `useProfileView().open(...)`. Someone else's has Message and Ignore user when logged in with online play
 * set up (the chat's own actions). The player's own has a pencil beside the name: edit mode (name, mains, country,
 * social media), where every change is saved as it is made; a check beside the X leaves it. Under it are Switch account
 * and Log out (the header's name opens it: there is no account menu), and Activate online play / Verify email until
 * they're done. Until the player has clicked that pencil once on this client, it glows gold.
 */
export const ProfileDialog = React.memo(function ProfileDialog() {
  const target = useProfileView((s) => s.target);
  const close = useProfileView((s) => s.close);
  return (
    <Dialog open={target != null} onClose={close} maxWidth="xs" fullWidth={true}>
      {target && <ProfileDialogBody uid={target.uid} seedName={target.displayName} onClose={close} />}
    </Dialog>
  );
});

const ProfileDialogBody = ({ uid, seedName, onClose }: { uid: string; seedName?: string; onClose: () => void }) => {
  const { backendService } = useServices();
  const { showSuccess } = useToasts();
  const myUid = useAccount((s) => s.user?.uid);
  const [editMode, setEditMode] = useState(false);
  const editing = useProfileEditing(uid);
  // Fresh each time it opens (the rating changes after every set).
  const { data, error, isLoading } = useQuery({
    queryKey: ["profile", uid],
    queryFn: () => backendService.fetchProfile(uid),
    refetchOnMount: "always",
  });
  const own = myUid != null && myUid === uid;
  const pencilFound = useOwnProfileHint((s) => s.opened);
  const markPencilFound = useOwnProfileHint((s) => s.markOpened);
  return (
    <div className={styles.dialog}>
      <div className={styles.corner}>
        {own && data && editMode && (
          <Tooltip title={Messages.doneEditing()}>
            <IconButton
              size="small"
              aria-label={Messages.doneEditing()}
              aria-pressed={true}
              className={styles.editOn}
              onClick={() => setEditMode(false)}
              data-testid="profile-edit"
            >
              <CheckIcon fontSize="small" />
            </IconButton>
          </Tooltip>
        )}
        <IconButton size="small" aria-label={Messages.close()} onClick={onClose}>
          <CloseIcon fontSize="small" />
        </IconButton>
      </div>
      {data ? (
        <ProfileView
          profile={data}
          actions={<ProfileActions profile={data} onDone={onClose} />}
          editing={own && editMode ? editing : null}
          editToggle={
            own && !editMode ? (
              <EditPencil
                glow={!pencilFound}
                onClick={() => {
                  setEditMode(true);
                  if (!pencilFound) {
                    markPencilFound();
                  }
                }}
              />
            ) : undefined
          }
          onCopyLink={(link) => void navigator.clipboard.writeText(link).then(() => showSuccess(Messages.linkCopied()))}
          onCopyText={(text) =>
            void navigator.clipboard.writeText(text).then(() => showSuccess(Messages.discordCopied(text)))
          }
        />
      ) : isLoading ? (
        <div className={styles.status}>
          {seedName && <div className={styles.name}>{seedName}</div>}
          <CircularProgress size={24} />
        </div>
      ) : (
        <div className={styles.status}>
          {seedName && <div className={styles.name}>{seedName}</div>}
          <div>{error ? Messages.couldNotLoad() : Messages.notFound()}</div>
        </div>
      )}
      {own && <OwnAccountActions ruled={!data?.link} onClose={onClose} />}
    </div>
  );
};

/** The pencil beside the player's own name that opens edit mode; it glows gold until they have clicked it once. */
export const EditPencil = ({ glow, onClick }: { glow: boolean; onClick: () => void }) => (
  <Tooltip title={Messages.editProfile()}>
    <IconButton
      size="small"
      aria-label={Messages.editProfile()}
      aria-pressed={false}
      className={clsx(glow && styles.pencilGlow)}
      onClick={onClick}
      data-testid="profile-edit"
    >
      <EditOutlinedIcon fontSize="small" />
    </IconButton>
  </Tooltip>
);

/** The player's own profile's account actions; each closes the profile and opens its dialog (user_menu.tsx). */
const OwnAccountActions = ({ ruled, onClose }: { ruled: boolean; onClose: () => void }) => {
  const show = useAccountDialogs((s) => s.show);
  const activated = useAccount((s) => s.userData?.playKey != null);
  const emailVerified = useAccount((s) => s.user?.emailVerified ?? true);
  const serverError = useAccount((s) => s.serverError);
  return (
    <OwnAccountButtons
      activate={!activated && !serverError}
      verify={!emailVerified}
      ruled={ruled}
      onOpen={(dialog) => {
        onClose();
        show(dialog);
      }}
    />
  );
};

/**
 * Under the player's own profile: Switch account and Log out on the right, below the web link, and above them
 * Activate online play / Verify email until they're done (also the stories'). `ruled`: no web link above, so a rule.
 */
export const OwnAccountButtons = ({
  activate,
  verify,
  ruled = false,
  onOpen,
}: {
  activate: boolean;
  verify: boolean;
  ruled?: boolean;
  onOpen: (dialog: AccountDialog) => void;
}) => (
  <div className={clsx(styles.account, ruled && styles.ruled)} data-testid="profile-account">
    {(activate || verify) && (
      <div className={styles.todo}>
        {activate && (
          <Button variant="contained" size="small" onClick={() => onOpen("activate")}>
            {Messages.activateOnlinePlay()}
          </Button>
        )}
        {verify && (
          <Button variant="contained" size="small" onClick={() => onOpen("verify")}>
            {Messages.verifyEmail()}
          </Button>
        )}
      </div>
    )}
    <Button size="small" className={styles.accountLink} onClick={() => onOpen("switch")}>
      {Messages.switchAccount()}
    </Button>
    <Button size="small" className={styles.accountLink} onClick={() => onOpen("logout")}>
      {Messages.logOut()}
    </Button>
  </div>
);

/**
 * Edit mode's server calls for the player's own profile: the name through the display-name rename path (the slur filter
 * applies; the header's name follows), the rest through `PUT /v1/me/profile`. The profile shown takes the answer; a
 * refusal is a toast with the server's message.
 */
function useProfileEditing(uid: string): ProfileEditing {
  const { backendService } = useServices();
  const { showError } = useToasts();
  const queryClient = useQueryClient();
  const setDisplayName = useAccount((s) => s.setDisplayName);
  return useMemo(
    () => ({
      rename: async (name: string) => {
        try {
          await backendService.changeDisplayName(name);
          setDisplayName(name);
        } catch (err) {
          showError(err);
          throw err;
        } finally {
          await queryClient.invalidateQueries({ queryKey: ["profile", uid] });
        }
      },
      save: async (update: ProfileUpdate) => {
        try {
          const profile = await backendService.updateProfile(update);
          queryClient.setQueryData(["profile", uid], profile);
        } catch (err) {
          showError(err);
          throw err;
        }
      },
    }),
    [backendService, queryClient, setDisplayName, showError, uid],
  );
}

/** Message and Ignore user, for someone else's profile when the chat is there (logged in, online play set up). */
const ProfileActions = ({ profile, onDone }: { profile: PlayerProfile; onDone: () => void }) => {
  const myUid = useAccount((s) => s.user?.uid);
  const canChat = useAccount((s) => s.userData?.playKey != null);
  const ignored = useSocial((s) => s.state.ignored.includes(profile.uid));
  const { message, setIgnored } = useSocialActions();
  if (!myUid || !canChat || myUid === profile.uid) {
    return null;
  }
  const player = { uid: profile.uid, displayName: profile.displayName, connectCode: profile.connectCode };
  return (
    <ProfileButtons
      ignored={ignored}
      onMessage={() => {
        message(player);
        onDone();
      }}
      onIgnore={() => void setIgnored(player, !ignored)}
    />
  );
};

/** The Message and Ignore user buttons themselves (also the stories'). */
export const ProfileButtons = ({
  ignored,
  onMessage,
  onIgnore,
}: {
  ignored: boolean;
  onMessage: () => void;
  onIgnore: () => void;
}) => (
  <>
    <Button variant="contained" size="small" startIcon={<MailOutlinedIcon />} onClick={onMessage}>
      {Messages.message()}
    </Button>
    <Button
      variant="outlined"
      size="small"
      color={ignored ? "inherit" : "secondary"}
      startIcon={<BlockIcon />}
      onClick={onIgnore}
    >
      {ignored ? Messages.unignoreUser() : Messages.ignoreUser()}
    </Button>
  </>
);
