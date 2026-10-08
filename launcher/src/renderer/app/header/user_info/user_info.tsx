import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import CircularProgress from "@mui/material/CircularProgress";
import { clsx } from "clsx";
import React from "react";

import { UserIcon } from "@/components/user_icon";

import styles from "./user_info.module.css";

export const UserInfo = React.memo(function UserInfo({
  displayName,
  displayPicture,
  connectCode,
  errorBorder,
  errorMessage,
  loading,
}: {
  displayName: string;
  displayPicture: string;
  connectCode?: string;
  errorBorder?: boolean;
  errorMessage?: string;
  loading?: boolean;
}) {
  return (
    <div className={styles.root}>
      {loading ? (
        <CircularProgress color="inherit" />
      ) : (
        <UserIcon imageUrl={displayPicture} size={42} borderColor={errorBorder ? "var(--red-error)" : undefined} />
      )}
      <div className={styles.content}>
        <h3 className={styles.displayName}>{displayName}</h3>
        {!loading &&
          (errorMessage ? (
            <div className={clsx(styles.subtitle, styles.error)}>{errorMessage}</div>
          ) : (
            <div className={styles.subtitle}>
              <span>{connectCode}</span>
            </div>
          ))}
      </div>
      <div className={styles.expandMoreIcon}>
        <ExpandMoreIcon />
      </div>
    </div>
  );
});
