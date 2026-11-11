import CircularProgress from "@mui/material/CircularProgress";
import { clsx } from "clsx";
import React from "react";

import { MainPicture } from "@/components/main_picture/main_picture";

import styles from "./user_info.module.css";

export const UserInfo = React.memo(function UserInfo({
  displayName,
  picture,
  connectCode,
  errorBorder,
  errorMessage,
  loading,
}: {
  displayName: string;
  /** The primary main's stock icon; null for none (Fox's outline, a pencil on hover); undefined while loading. */
  picture: string | null | undefined;
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
        <MainPicture url={picture} size={42} pencil={true} error={errorBorder} />
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
    </div>
  );
});
