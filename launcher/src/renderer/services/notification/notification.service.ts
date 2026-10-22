import { withoutIpcPrefix } from "@common/disk_space";
import { enqueueSnackbar } from "notistack";

import type { NotificationService } from "./types";

export default function createNotificationClient(): NotificationService {
  return {
    showError: (error: Error | string | unknown) => {
      let message: string;
      if (error instanceof Error) {
        message = error.message;
      } else if (typeof error === "string") {
        message = error;
      } else {
        message = JSON.stringify(error);
      }
      // Electron's "Error invoking remote method '…': Error: " means nothing to a player.
      message = withoutIpcPrefix(message);
      // Errors stay up longer than the other toasts, but still go away on their own: most are transient
      // ("Close Dolphin to switch accounts", a chat message that didn't send) and used to stay until closed.
      enqueueSnackbar(message, { variant: "error", autoHideDuration: 10000 });
    },
    showSuccess: (message: string) => enqueueSnackbar(message, { variant: "success", autoHideDuration: 2500 }),
    showWarning: (message: string) => enqueueSnackbar(message, { variant: "warning" }),
    showInfo: (message: string) => enqueueSnackbar(message, { variant: "info" }),
  };
}
