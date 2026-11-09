import BlockIcon from "@mui/icons-material/Block";
import CloseIcon from "@mui/icons-material/Close";
import MailOutlinedIcon from "@mui/icons-material/MailOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import IconButton from "@mui/material/IconButton";
import { useQuery } from "@tanstack/react-query";
import React from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useSocial } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";
import { useServices } from "@/services";
import type { PlayerProfile } from "@/services/backend/types";

import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";
import { ProfileView } from "./profile_view";

/**
 * View Profile (docs/design/profiles.md): anyone's public profile, over whatever page is open. Opened with
 * `useProfileView().open(...)`. Someone else's has Message and Ignore user when logged in with online play
 * set up (the chat's own actions).
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
  // Fresh each time it opens (the rating changes after every set).
  const { data, error, isLoading } = useQuery({
    queryKey: ["profile", uid],
    queryFn: () => backendService.fetchProfile(uid),
    refetchOnMount: "always",
  });
  return (
    <div className={styles.dialog}>
      <IconButton className={styles.close} size="small" aria-label={Messages.close()} onClick={onClose}>
        <CloseIcon fontSize="small" />
      </IconButton>
      {data ? (
        <ProfileView
          profile={data}
          actions={<ProfileActions profile={data} onDone={onClose} />}
          onCopyLink={(link) => void navigator.clipboard.writeText(link).then(() => showSuccess(Messages.linkCopied()))}
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
    </div>
  );
};

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
    <>
      <Button
        variant="contained"
        size="small"
        startIcon={<MailOutlinedIcon />}
        onClick={() => {
          message(player);
          onDone();
        }}
      >
        {Messages.message()}
      </Button>
      <Button
        variant="outlined"
        size="small"
        color={ignored ? "inherit" : "secondary"}
        startIcon={<BlockIcon />}
        onClick={() => void setIgnored(player, !ignored)}
      >
        {ignored ? Messages.unignoreUser() : Messages.ignoreUser()}
      </Button>
    </>
  );
};
