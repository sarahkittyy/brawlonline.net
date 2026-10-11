import Alert from "@mui/material/Alert";
import type { CustomContentProps } from "notistack";
import { SnackbarContent, SnackbarProvider, useSnackbar } from "notistack";
import React, { forwardRef, memo } from "react";

const CustomSnackbarContent = forwardRef<HTMLDivElement, CustomContentProps>((props, forwardedRef) => {
  const { closeSnackbar } = useSnackbar();

  const { id, message, variant } = props;

  return (
    <SnackbarContent ref={forwardedRef}>
      <Alert
        variant="standard"
        severity={variant === "default" ? "info" : variant}
        onClose={() => closeSnackbar(id)}
        sx={{
          maxWidth: 500,
          width: "100%",
        }}
      >
        {message}
      </Alert>
    </SnackbarContent>
  );
});

const CustomToast = memo(CustomSnackbarContent);

export const ToastProvider = ({ children }: { children: React.ReactNode }) => {
  return (
    <SnackbarProvider
      maxSnack={3}
      preventDuplicate={true}
      // notistack stops every toast's timer while the window is unfocused, and the launcher is unfocused
      // whenever the game is, so toasts shown around a hand-off stayed until the launcher was clicked again.
      // Hovering a toast still holds it.
      disableWindowBlurListener={true}
      anchorOrigin={{
        vertical: "bottom",
        horizontal: "right",
      }}
      Components={{
        default: CustomToast,
        info: CustomToast,
        success: CustomToast,
        error: CustomToast,
        warning: CustomToast,
      }}
      TransitionProps={{
        direction: "up",
        mountOnEnter: true,
        unmountOnExit: true,
      }}
    >
      {children}
    </SnackbarProvider>
  );
};
