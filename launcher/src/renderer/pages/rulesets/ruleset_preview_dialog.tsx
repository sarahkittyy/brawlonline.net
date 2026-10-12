import type { RulesetView } from "@accounts/types";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import { useQuery } from "@tanstack/react-query";

import { useToasts } from "@/lib/hooks/use_toasts";
import { rulesetTitle, useRememberedRuleset } from "@/lib/rulesets/use_remembered_ruleset";
import { useServices } from "@/services";

import { RulesetDetails } from "./ruleset_details";
import { RulesetsPageMessages as Messages } from "./rulesets_page.messages";
import styles from "./rulesets_page.module.css";

/**
 * A published ruleset in a dialog: the Rulesets page's read-only view (ruleset_details.tsx) with copy code / link and
 * Use for my rooms. Give it the ruleset (`view`) or its `code` (fetched when the dialog opens). Esc or the back arrow
 * closes it.
 */
export const RulesetPreviewDialog = ({
  open,
  onClose,
  code,
  view,
}: {
  open: boolean;
  onClose: () => void;
  code?: string | null;
  view?: RulesetView | null;
}) => {
  const { backendService } = useServices();
  const { showError, showSuccess } = useToasts();
  const remembered = useRememberedRuleset();
  const query = useQuery({
    queryKey: ["ruleset", code],
    queryFn: () => backendService.fetchRuleset(code!),
    enabled: open && !view && !!code,
    retry: false,
  });
  const shown = view ?? query.data;

  const copy = (text: string) => {
    void navigator.clipboard.writeText(text).then(() => showSuccess(Messages.copied()));
  };
  const use = (c: string) => {
    remembered.setRemembered(c).catch(showError);
  };

  return (
    <Dialog open={open} onClose={onClose} fullWidth={true} maxWidth="md" data-testid="ruleset-preview-dialog">
      <DialogTitle sx={{ display: "flex", alignItems: "center", gap: 1 }}>
        <IconButton size="small" onClick={onClose} aria-label={Messages.back()} data-testid="ruleset-preview-back">
          <ArrowBackIcon fontSize="small" />
        </IconButton>
        <span className={styles.previewTitle}>{shown ? rulesetTitle(shown) : code ?? ""}</span>
        {shown && <span className={styles.code}>{shown.code}</span>}
      </DialogTitle>
      <DialogContent dividers={true}>
        {shown ? (
          <RulesetDetails ruleset={shown.ruleset} />
        ) : query.isError ? (
          <p className={styles.problem}>{Messages.lookupFailed()}</p>
        ) : (
          <div className={styles.inlineStatus}>
            <CircularProgress color="inherit" size={20} />
          </div>
        )}
      </DialogContent>
      {shown && (
        <DialogActions sx={{ justifyContent: "flex-start", px: 2 }}>
          <Tooltip title={Messages.copyCode()}>
            <IconButton size="small" onClick={() => copy(shown.code)} aria-label={Messages.copyCode()}>
              <ContentCopyIcon fontSize="small" />
            </IconButton>
          </Tooltip>
          <Button color="secondary" sx={{ textTransform: "none" }} onClick={() => copy(shown.link)}>
            {Messages.copyLink()}
          </Button>
          {remembered.ruleset?.code === shown.code ? (
            <span className={styles.inUse}>{Messages.inUse()}</span>
          ) : (
            <Button
              color="secondary"
              sx={{ textTransform: "none" }}
              onClick={() => use(shown.code)}
              data-testid="preview-use"
            >
              {Messages.forMyRooms()}
            </Button>
          )}
        </DialogActions>
      )}
    </Dialog>
  );
};
