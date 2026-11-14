import ChevronLeftRoundedIcon from "@mui/icons-material/ChevronLeftRounded";
import ButtonBase from "@mui/material/ButtonBase";
import React, { useEffect } from "react";
import { useNavigate, useParams } from "react-router-dom";

import { ProfileDialogBody } from "@/components/profile/profile_dialog";
import { useAccount } from "@/lib/hooks/use_account";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { MatchHistory } from "@/pages/match_history/match_history_page";

import styles from "./profile_page.module.css";

/** The profile page's address. */
export const profilePath = (uid: string) => `/main/profile/${encodeURIComponent(uid)}`;

/**
 * A player's profile as a page (docs/design/launcher-redesign.md §2), no longer a dialog over the page: the profile
 * (edit mode, the friend pill, Invite to game and Message for someone else, Switch account and Log out for the player's
 * own) and under it the match history: the player's own, or their sets with this player (match history is private).
 */
export const ProfilePage = React.memo(function ProfilePage() {
  const uid = decodeURIComponent(useParams()["*"] ?? "");
  const myUid = useAccount((s) => s.user?.uid);
  const navigate = useNavigate();
  if (!uid) {
    return null;
  }
  const own = myUid === uid;
  return (
    <div className={styles.page}>
      <ButtonBase className={styles.back} onClick={() => navigate(-1)}>
        <ChevronLeftRoundedIcon fontSize="small" />
      </ButtonBase>
      <section className={styles.hero} data-testid="profile-page">
        <ProfileDialogBody key={uid} uid={uid} onClose={() => navigate(-1)} page={true} />
      </section>
      {myUid && (
        <section className={styles.history}>
          <MatchHistory uid={myUid} opponent={own ? undefined : uid} />
        </section>
      )}
    </div>
  );
});

/**
 * Wherever a name opens a profile (`useProfileView().open`: the player's card, the right-click menu, a chat title, the
 * leaderboard, the match history), it now opens the profile page.
 */
export const ProfileNavigator = () => {
  const target = useProfileView((s) => s.target);
  const close = useProfileView((s) => s.close);
  const navigate = useNavigate();
  useEffect(() => {
    if (target) {
      navigate(profilePath(target.uid));
      close();
    }
  }, [target, navigate, close]);
  return null;
};
