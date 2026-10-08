import styled from "@emotion/styled";
import React from "react";

/**
 * The launcher's version ("v0.1.27") in the bottom right corner of the window, on every page.
 * `__VERSION__` is release/app/package.json's version, the same file `app.getVersion()` reads
 * (CI stamps it before the build). It never takes clicks, and footer bars leave room for it
 * (components/footer/footer.tsx), so it covers nothing.
 */
export const VersionLabel = React.memo(() => {
  return <Label>v{__VERSION__}</Label>;
});

/** Room a footer bar keeps free on its right for the label. */
export const VERSION_LABEL_SPACE = "70px";

const Label = styled.div`
  position: fixed;
  right: 8px;
  bottom: 4px;
  z-index: 1000; /* above the pages, below MUI's dialogs, snackbars and tooltips */
  pointer-events: none;
  user-select: none;
  color: rgba(255, 255, 255, 0.3);
  font-size: 11px;
  line-height: 14px;
`;
