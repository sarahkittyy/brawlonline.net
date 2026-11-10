import ButtonBase from "@mui/material/ButtonBase";
import DialogContentText from "@mui/material/DialogContentText";
import { clsx } from "clsx";
import log from "electron-log";
import React from "react";

import { ConfirmationModal } from "@/components/confirmation_modal/confirmation_modal";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useAccount } from "@/lib/hooks/use_account";
import { useAccountDialogs } from "@/lib/hooks/use_account_dialogs";
import { useAppStore } from "@/lib/hooks/use_app_store";
import { useOwnProfileHint } from "@/lib/hooks/use_own_profile_hint";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";
import type { AuthUser } from "@/services/auth/types";
import { SessionExpiredError } from "@/services/auth/types";

import { AddAccountDialog } from "../account_switcher/add_account_dialog";
import { SwitchAccountDialog } from "../account_switcher/switch_account_dialog";
import { ActivateOnlineDialog } from "../activate_online_dialog";
import { UserInfo } from "../user_info/user_info";
import { VerifyEmailDialog } from "../verify_email_dialog";
import { UserMenuMessages as Messages } from "./user_menu.messages";
import styles from "./user_menu.module.css";

const MAX_ACCOUNTS = 5;

/**
 * The account in the header: clicking it opens the player's own profile, whose footer has Switch account and Log out
 * (and Activate online play / Verify email until they're done). Those dialogs are here (use_account_dialogs.ts).
 */
export const UserMenu = ({ user, handleError }: { user: AuthUser; handleError: (error: any) => void }) => {
  const { authService, backendService, dolphinService } = useServices();
  const userData = useAccount((store) => store.userData);
  const displayName = useAccount((store) => store.displayName);
  const loading = useAccount((store) => store.loading);
  const serverError = useAccount((store) => store.serverError);
  const accounts = useAccount((store) => store.accounts);
  const activeAccountId = useAccount((store) => store.activeAccountId);
  const setAccounts = useAccount((store) => store.setAccounts);
  const setActiveAccountId = useAccount((store) => store.setActiveAccountId);
  const netplayOpened = useDolphinStore((store) => store.netplayOpened);
  const { showSuccess, showError } = useToasts();
  const openProfile = useProfileView((store) => store.open);
  const ownProfileOpened = useOwnProfileHint((store) => store.opened);
  const accountDialog = useAccountDialogs((store) => store.open);
  const closeAccountDialog = useAccountDialogs((store) => store.close);

  const [openAddAccountDialog, setOpenAddAccountDialog] = React.useState(false);
  const [switching, setSwitching] = React.useState(false);
  const [reAuthEmail, setReAuthEmail] = React.useState<string | undefined>();
  const isOnline = useAppStore((state) => state.isOnline);

  const onLogout = async () => {
    try {
      await authService.logout();
    } catch (err) {
      log.error(err);
      handleError(err);
    } finally {
      closeAccountDialog();
    }
  };

  // Get multi-account service
  const multiAccountService = authService.getMultiAccountService();

  // Handle account switch
  const handleSwitchAccount = async (accountId: string) => {
    if (switching) {
      return;
    }

    // A Dolphin open without a game closes; one in a game would log in as the other account.
    if (netplayOpened && !(await dolphinService.closeNetplayIfIdle().catch(() => false))) {
      showError(Messages.closeDolphinToSwitchAccounts());
      return;
    }

    setSwitching(true);

    try {
      await multiAccountService.switchAccount(accountId);

      // Update local state
      const accountsList = multiAccountService.getAccounts();
      const activeId = multiAccountService.getActiveAccountId();
      setAccounts(accountsList);
      setActiveAccountId(activeId);

      const account = accountsList.find((acc) => acc.id === accountId);
      if (account) {
        showSuccess(Messages.switchedTo(account.displayName || account.id));
      }
      closeAccountDialog();

      // Refresh user data for new account
      await backendService.fetchUserData().catch(() => {
        // Ignore errors here - user data will be fetched by other listeners
      });
    } catch (err: any) {
      // Check if session expired
      if (err instanceof SessionExpiredError) {
        // Show login dialog with pre-filled email
        closeAccountDialog();
        setReAuthEmail(err.email);
        setOpenAddAccountDialog(true);
      } else {
        showError(err?.message || Messages.failedToSwitchAccount());
        log.error("Failed to switch account:", err);
      }
    } finally {
      setSwitching(false);
    }
  };

  // Handle add account
  const handleAddAccount = () => {
    if (accounts.length >= MAX_ACCOUNTS) {
      showError(Messages.maxAccountsReached(MAX_ACCOUNTS));
      return;
    }

    closeAccountDialog();
    setOpenAddAccountDialog(true);
  };

  // Handle remove account
  const handleRemoveAccount = async (accountId: string) => {
    try {
      const account = accounts.find((acc) => acc.id === accountId);
      await multiAccountService.removeAccount(accountId);

      // Update local state
      const accountsList = multiAccountService.getAccounts();
      const activeId = multiAccountService.getActiveAccountId();
      setAccounts(accountsList);
      setActiveAccountId(activeId);

      if (account) {
        showSuccess(Messages.accountRemoved(account.displayName || account.id));
      }
    } catch (err: any) {
      showError(err?.message || Messages.failedToRemoveAccount());
      log.error("Failed to remove account:", err);
    }
  };

  let isUserErrorMessage: boolean = false;
  let errMessage: string | undefined = undefined;
  if (!isOnline) {
    errMessage = Messages.offline();
  } else if (serverError) {
    errMessage = Messages.serverError();
  } else if (!userData?.playKey) {
    errMessage = Messages.onlineActivationRequired();
    isUserErrorMessage = true;
  } else if (!user?.emailVerified) {
    errMessage = Messages.emailVerificationRequired();
    isUserErrorMessage = true;
  }

  // Until this client's player has clicked their own profile's edit pencil: the name glows gold, then the pencil.
  const glowProfile = !ownProfileOpened && !!userData?.playKey && !errMessage && !loading;

  return (
    <div>
      <ButtonBase
        onClick={() => openProfile({ uid: user.uid, displayName, connectCode: userData?.playKey?.connectCode })}
        className={clsx(styles.userInfoButton, glowProfile && styles.profileGlow)}
      >
        <UserInfo
          displayName={displayName}
          displayPicture={user.displayPicture}
          connectCode={userData?.playKey?.connectCode}
          errorBorder={isUserErrorMessage}
          errorMessage={errMessage}
          loading={loading}
        />
      </ButtonBase>

      <SwitchAccountDialog
        open={accountDialog === "switch"}
        onClose={closeAccountDialog}
        accounts={accounts}
        activeAccountId={activeAccountId}
        onSwitchAccount={handleSwitchAccount}
        onAddAccount={handleAddAccount}
        onRemoveAccount={handleRemoveAccount}
        switching={switching}
      />
      <VerifyEmailDialog open={accountDialog === "verify"} onClose={closeAccountDialog} />
      <ActivateOnlineDialog
        open={accountDialog === "activate"}
        onClose={closeAccountDialog}
        onSubmit={closeAccountDialog}
      />
      <ConfirmationModal
        title={Messages.areYouSureYouWantToLogout()}
        confirmText={Messages.logout()}
        open={accountDialog === "logout"}
        onClose={closeAccountDialog}
        onSubmit={onLogout}
        fullWidth={false}
      >
        <DialogContentText>{Messages.youWillNeedToLogInAgain()}</DialogContentText>
      </ConfirmationModal>

      {/* Multi-Account Dialogs */}
      <AddAccountDialog
        open={openAddAccountDialog}
        onClose={() => {
          setOpenAddAccountDialog(false);
          setReAuthEmail(undefined);
        }}
        onSuccess={() => {
          if (multiAccountService) {
            const accountsList = multiAccountService.getAccounts();
            const activeId = multiAccountService.getActiveAccountId();
            setAccounts(accountsList);
            setActiveAccountId(activeId);
          }
          setReAuthEmail(undefined);
        }}
        defaultEmail={reAuthEmail}
      />
    </div>
  );
};
