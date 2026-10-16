import { css } from "@emotion/react";
import BugReportOutlinedIcon from "@mui/icons-material/BugReportOutlined";
import DownloadIcon from "@mui/icons-material/Download";
import LiveHelpIcon from "@mui/icons-material/LiveHelp";
import log from "electron-log";
import { useState } from "react";

import { BugReportDialog } from "@/app/header/bug_report/bug_report_button";
import { Button } from "@/components/form/button";
import { useToasts } from "@/lib/hooks/use_toasts";

import { NetworkDiagnosticsButton } from "./network_diagnostics_button/network_diagnostics_button";
import { SupportBoxMessages as Messages } from "./support_box.messages";
import styles from "./support_box.module.css";

export const SupportBox = () => {
  const { showError, showSuccess } = useToasts();

  const [saving, setSaving] = useState(false);
  const [reportOpen, setReportOpen] = useState(false);

  const onDownload = () => {
    setSaving(true);
    window.electron.common
      .downloadLogs()
      .then((zipPath) => {
        if (zipPath) {
          showSuccess(Messages.logsSaved(zipPath));
        }
      })
      .catch((err: unknown) => {
        log.error(err);
        showError(err);
      })
      .finally(() => setSaving(false));
  };

  return (
    <div className={styles.container}>
      <h2 className={styles.iconContainer}>
        <LiveHelpIcon className={styles.helpIcon} />
        {Messages.needHelp()}
      </h2>
      <div>{Messages.bestWayToGetSupport()}</div>
      <div
        css={css`
          margin-top: 5px;
          & > div {
            display: inline-block;
            margin-top: 10px;
            margin-right: 10px;
          }
        `}
      >
        <div>
          <Button startIcon={<BugReportOutlinedIcon />} onClick={() => setReportOpen(true)}>
            {Messages.reportBug()}
          </Button>
        </div>
        <div>
          <Button startIcon={<DownloadIcon />} onClick={onDownload} disabled={saving}>
            {saving ? Messages.savingLogs() : Messages.downloadLogs()}
          </Button>
        </div>
        <div>
          <NetworkDiagnosticsButton />
        </div>
      </div>
      <BugReportDialog open={reportOpen} onClose={() => setReportOpen(false)} />
    </div>
  );
};
