import CheckCircleOutlineIcon from "@mui/icons-material/CheckCircleOutline";
import Button from "@mui/material/Button";
import TextField from "@mui/material/TextField";
import { useEffect, useState } from "react";

import { useLocalStorage } from "@/lib/hooks/use_local_storage";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";
import type { AuthUser } from "@/services/auth/types";

import { VerifyEmailFormMessages as Messages } from "./verify_email_form.messages";
import styles from "./verify_email_form.module.css";

const VERIFICATION_EMAIL_SENT_KEY = "verificationEmailSentByUser";

export function VerifyEmailForm({ user }: { user: AuthUser }) {
  const { authService } = useServices();
  const { showError } = useToasts();
  const [sentByUser, setSentByUser] = useLocalStorage<Record<string, boolean>>(VERIFICATION_EMAIL_SENT_KEY, {});
  const emailVerificationSent = sentByUser[user.uid] ?? false;

  const handleCheckVerification = async () => {
    try {
      await authService.refreshUser();

      // Get current user manually since the user variable above hasn't updated yet
      const newUser = authService.getCurrentUser();
      if (!newUser?.emailVerified) {
        showError(Messages.emailIsNotVerified());
      }
    } catch (err: any) {
      showError(err.message);
    }
  };

  useEffect(() => {
    const sendVerificationEmail = async () => {
      try {
        await authService.sendVerificationEmail();
        setSentByUser((prev) => ({ ...prev, [user.uid]: true }));
      } catch (err: any) {
        showError(err.message);
      }
    };

    if (user && !user.emailVerified && !emailVerificationSent) {
      void sendVerificationEmail();
    }
  }, [emailVerificationSent, setSentByUser, showError, user, authService]);

  const preVerification = (
    <>
      <div className={styles.instructions}>{Messages.visitYourEmail()}</div>
      <Button variant="outlined" onClick={handleCheckVerification}>
        {Messages.checkVerification()}
      </Button>
      <div className={styles.emailNotFoundContainer}>
        {Messages.cantFindEmail()}{" "}
        <a
          href="#"
          onClick={(e) => {
            e.preventDefault();
            void authService.sendVerificationEmail();
          }}
        >
          {Messages.sendAgain()}
        </a>
      </div>
    </>
  );

  const postVerification = (
    <div className={styles.confirmationContainer}>
      <CheckCircleOutlineIcon />
      {Messages.emailVerified()}
    </div>
  );

  return (
    <div>
      <div className={styles.message}>{Messages.aConfirmationEmailHasBeenSentTo()}</div>
      <div className={styles.emailContainer}>{user.email}</div>
      {/* Slippi links "Wrong email? Change email" to slippi.gg/manage; ours changes it here (bug #163). */}
      {!user.emailVerified && <ChangeEmail email={user.email} />}
      {user.emailVerified ? postVerification : preVerification}
    </div>
  );
}

/** "Wrong email? Change email": a typo at sign-up, fixed before the email is verified. */
function ChangeEmail({ email }: { email: string }) {
  const { authService } = useServices();
  const { showError } = useToasts();
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(email);
  const [saving, setSaving] = useState(false);

  const submit = async () => {
    setSaving(true);
    try {
      await authService.changeEmail(value);
      setEditing(false);
    } catch (err: any) {
      showError(err.message);
    } finally {
      setSaving(false);
    }
  };

  if (!editing) {
    return (
      <div className={styles.incorrectEmailContainer}>
        {Messages.wrongEmail()}{" "}
        <a
          href="#"
          onClick={(e) => {
            e.preventDefault();
            setValue(email);
            setEditing(true);
          }}
        >
          {Messages.changeEmail()}
        </a>
      </div>
    );
  }

  return (
    <form
      className={styles.changeEmailForm}
      onSubmit={(e) => {
        e.preventDefault();
        void submit();
      }}
    >
      <TextField
        label={Messages.newEmail()}
        variant="filled"
        size="small"
        type="email"
        value={value}
        autoFocus={true}
        disabled={saving}
        required={true}
        onChange={(e) => setValue(e.target.value)}
      />
      <Button type="submit" variant="contained" disabled={saving || value.trim() === ""}>
        {Messages.change()}
      </Button>
      <Button disabled={saving} onClick={() => setEditing(false)}>
        {Messages.cancel()}
      </Button>
    </form>
  );
}
