import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import React from "react";

import type { CursorPages } from "@/lib/hooks/use_cursor_pages";

import { PagedListStatusMessages as Messages } from "./paged_list_status.messages";

/**
 * The line under a paged list: a spinner while a page loads, the wait while a rate-limited
 * page is retried, or the error with a Retry button. Nothing once the list is idle.
 */
export const PagedListStatus = ({
  pages,
}: {
  pages: Pick<CursorPages<unknown>, "isLoading" | "isLoadingMore" | "rateLimitedFor" | "error" | "retry">;
}) => {
  const { isLoading, isLoadingMore, rateLimitedFor, error, retry } = pages;
  let content: React.ReactNode = null;
  if (rateLimitedFor != null) {
    content = <span>{Messages.rateLimited(rateLimitedFor)}</span>;
  } else if (error) {
    content = (
      <>
        <span>{Messages.failed(error.message)}</span>
        <Button size="small" color="inherit" onClick={retry}>
          {Messages.retry()}
        </Button>
      </>
    );
  } else if (isLoading || isLoadingMore) {
    content = <CircularProgress color="inherit" size={20} />;
  }
  if (content == null) {
    return null;
  }
  return (
    <div
      role="status"
      style={{
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        gap: 8,
        padding: 12,
        fontSize: 13,
        color: "var(--text-secondary)",
      }}
    >
      {content}
    </div>
  );
};
