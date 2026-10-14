import { css } from "@emotion/react";
import BugReportOutlinedIcon from "@mui/icons-material/BugReportOutlined";
import CircularProgress from "@mui/material/CircularProgress";
import DialogContentText from "@mui/material/DialogContentText";
import IconButton from "@mui/material/IconButton";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import log from "electron-log";
import React from "react";

import { ConfirmationModal } from "@/components/confirmation_modal/confirmation_modal";
import { useAccount } from "@/lib/hooks/use_account";
import { useToasts } from "@/lib/hooks/use_toasts";

import { BugReportMessages as Messages } from "./bug_report.messages";

/** The server keeps at most this many characters (accounts `bug_reports::MAX_DESCRIPTION_CHARS`). */
const MAX_DESCRIPTION = 5000;

/**
 * The bug icon in the header: a popup where the player says what went wrong, sent to our server
 * with a zip of the launcher's and Dolphin's logs (`admin bug list` on the server). Works logged
 * out too, since logging in may be what is broken.
 */
export const BugReportButton = () => {
  const [open, setOpen] = React.useState(false);
  return (
    <>
      <Tooltip title={Messages.reportABug()}>
        <IconButton
          onClick={() => setOpen(true)}
          aria-label={Messages.reportABug()}
          css={css`
            opacity: 0.5;
          `}
          size="large"
        >
          <BugReportOutlinedIcon />
        </IconButton>
      </Tooltip>
      <BugReportDialog open={open} onClose={() => setOpen(false)} />
    </>
  );
};

export const BugReportDialog = ({ open, onClose }: { open: boolean; onClose: () => void }) => {
  const uid = useAccount((store) => store.user?.uid ?? null);
  const { showSuccess, showWarning, showError } = useToasts();
  const [description, setDescription] = React.useState("");
  const [sending, setSending] = React.useState(false);

  const onSubmit = () => {
    if (sending) {
      return;
    }
    setSending(true);
    window.electron.common
      .sendBugReport(uid, description.trim())
      .then((sent) => {
        if (sent.logsSent) {
          showSuccess(Messages.thanks());
        } else {
          showWarning(Messages.sentWithoutLogs(sent.logsError ?? ""));
        }
        setDescription("");
        onClose();
      })
      .catch((err: unknown) => {
        // The text did not get there: the popup stays open with it.
        log.error(err);
        showError(err);
      })
      .finally(() => setSending(false));
  };

  return (
    <ConfirmationModal
      title={Messages.bugReport()}
      open={open}
      onClose={onClose}
      onSubmit={onSubmit}
      closeOnSubmit={false}
      cancelText={Messages.cancel()}
      confirmProps={{ disabled: sending }}
      confirmText={
        sending ? (
          <span
            css={css`
              display: flex;
              align-items: center;
            `}
          >
            {Messages.uploading()}
            <CircularProgress
              size={16}
              color="inherit"
              css={css`
                margin-left: 10px;
              `}
            />
          </span>
        ) : (
          Messages.uploadLogs()
        )
      }
    >
      <DialogContentText
        css={css`
          margin-bottom: 16px;
        `}
      >
        {Messages.describeWhatWentWrong()}
      </DialogContentText>
      <TextField
        value={description}
        onChange={(e) => setDescription(e.target.value)}
        placeholder={Messages.placeholder()}
        hiddenLabel={true}
        sx={{ "& .MuiFilledInput-root": { padding: "7px 12px 12px" } }}
        multiline={true}
        minRows={6}
        maxRows={14}
        fullWidth={true}
        autoFocus={true}
        disabled={sending}
        inputProps={{ maxLength: MAX_DESCRIPTION, "aria-label": Messages.whatHappened() }}
      />
    </ConfirmationModal>
  );
};
