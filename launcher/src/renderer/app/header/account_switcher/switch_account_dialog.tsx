import CloseIcon from "@mui/icons-material/Close";
import Dialog from "@mui/material/Dialog";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import type { StoredAccount } from "@settings/types";

import { AccountSwitcher } from "./account_switcher";
import { AccountSwitcherMessages as Messages } from "./account_switcher.messages";

type SwitchAccountDialogProps = {
  open: boolean;
  onClose: () => void;
  accounts: readonly StoredAccount[];
  activeAccountId: string | null;
  onSwitchAccount: (accountId: string) => void;
  onAddAccount: () => void;
  onRemoveAccount: (accountId: string) => void;
  switching?: boolean;
};

/** Switch account, from the player's own profile: every account on this computer and "Add another account". */
export const SwitchAccountDialog = ({ open, onClose, ...switcher }: SwitchAccountDialogProps) => (
  <Dialog open={open} onClose={onClose} maxWidth="xs" fullWidth={true}>
    <DialogTitle sx={{ paddingRight: "48px" }}>{Messages.switchAccount()}</DialogTitle>
    <IconButton
      size="small"
      aria-label={Messages.close()}
      onClick={onClose}
      sx={{ position: "absolute", top: 12, right: 12 }}
    >
      <CloseIcon fontSize="small" />
    </IconButton>
    <DialogContent sx={{ padding: "0 8px 12px" }}>
      <AccountSwitcher {...switcher} />
    </DialogContent>
  </Dialog>
);
